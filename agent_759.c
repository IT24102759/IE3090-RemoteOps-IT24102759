#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

#define PORT 9410
#define BUFFER_SIZE 4096
#define AUTH_TOKEN "OPS-2759"
#define SID "9572"
#define LOG_FILE "remoteops_IT24102759.log"

#define STORAGE_PATH "./agentfiles/IT24102759"
#define MAX_FILE_SIZE (1024 * 1024)

#define MONITOR_INTERVAL_SEC 2

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    int client_socket;

    struct sockaddr_in client_address;

    int authenticated;
    int connection_open;

    pthread_t monitor_thread;

    int monitor_active;
    int monitor_udp_port;

    pthread_mutex_t monitor_mutex;

} ClientSession;

/*
 * Thread-safe timestamped logging.
 */
void log_event(const char *event)
{
    time_t current_time;
    struct tm time_info;
    char timestamp[64];

    time(&current_time);

    if (localtime_r(&current_time, &time_info) == NULL)
    {
        return;
    }

    strftime(timestamp,
             sizeof(timestamp),
             "%Y-%m-%d %H:%M:%S",
             &time_info);

    pthread_mutex_lock(&log_mutex);

    FILE *log_file = fopen(LOG_FILE, "a");

    if (log_file != NULL)
    {
        fprintf(log_file,
                "[%s] SID:%s %s\n",
                timestamp,
                SID,
                event);

        fclose(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}

/*
 * Send all bytes.
 */
int send_all(int socket_fd,
             const unsigned char *data,
             size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length)
    {
        ssize_t sent =
            send(socket_fd,
                 data + total_sent,
                 length - total_sent,
                 0);

        if (sent <= 0)
        {
            return -1;
        }

        total_sent += sent;
    }

    return 0;
}

/*
 * Send text response.
 */
int send_response(int client_socket,
                  const char *response)
{
    return send_all(client_socket,
                    (const unsigned char *)response,
                    strlen(response));
}

/*
 * Get system information.
 */
void get_system_info(double *cpu_load,
                     unsigned long *memory_used_mb,
                     unsigned long *uptime_sec)
{
    FILE *file;

    *cpu_load = 0.0;
    *memory_used_mb = 0;
    *uptime_sec = 0;

    /*
     * CPU load.
     */
    file = fopen("/proc/loadavg", "r");

    if (file != NULL)
    {
        fscanf(file, "%lf", cpu_load);
        fclose(file);
    }

    /*
     * Memory.
     */
    unsigned long mem_total_kb = 0;
    unsigned long mem_available_kb = 0;

    file = fopen("/proc/meminfo", "r");

    if (file != NULL)
    {
        char line[256];

        while (fgets(line, sizeof(line), file) != NULL)
        {
            sscanf(line,
                   "MemTotal: %lu kB",
                   &mem_total_kb);

            sscanf(line,
                   "MemAvailable: %lu kB",
                   &mem_available_kb);
        }

        fclose(file);
    }

    if (mem_total_kb >= mem_available_kb)
    {
        *memory_used_mb =
            (mem_total_kb - mem_available_kb) / 1024;
    }

    /*
     * Uptime.
     */
    double uptime_seconds = 0.0;

    file = fopen("/proc/uptime", "r");

    if (file != NULL)
    {
        fscanf(file, "%lf", &uptime_seconds);
        fclose(file);
    }

    *uptime_sec = (unsigned long)uptime_seconds;
}

/*
 * Get process list.
 */
int get_process_list(char *process_list,
                     size_t buffer_size)
{
    FILE *process_pipe =
        popen("ps -eo pid=,comm= --no-headers", "r");

    if (process_pipe == NULL)
    {
        return -1;
    }

    process_list[0] = '\0';

    char line[256];
    size_t used = 0;

    while (fgets(line, sizeof(line), process_pipe) != NULL)
    {
        int pid;
        char process_name[128];

        if (sscanf(line,
                   "%d %127s",
                   &pid,
                   process_name) != 2)
        {
            continue;
        }

        char entry[160];

        snprintf(entry,
                 sizeof(entry),
                 "%d/%s",
                 pid,
                 process_name);

        size_t entry_length = strlen(entry);

        if (used + entry_length + 2 >= buffer_size)
        {
            break;
        }

        if (used > 0)
        {
            process_list[used++] = ',';
            process_list[used] = '\0';
        }

        strcat(process_list, entry);

        used += entry_length;
    }

    pclose(process_pipe);

    return 0;
}

/*
 * Execute only allowed commands.
 */
int execute_allowed_command(const char *command,
                            char *output,
                            size_t output_size)
{
    const char *shell_command = NULL;

    if (strcmp(command, "DATE") == 0)
    {
        shell_command = "date";
    }
    else if (strcmp(command, "UPTIME") == 0)
    {
        shell_command = "uptime";
    }
    else if (strcmp(command, "DISKFREE") == 0)
    {
        shell_command = "df -h / | tail -1";
    }
    else if (strcmp(command, "HOSTNAME") == 0)
    {
        shell_command = "hostname";
    }
    else if (strcmp(command, "WHOAMI") == 0)
    {
        shell_command = "whoami";
    }
    else
    {
        return -1;
    }

    FILE *command_pipe =
        popen(shell_command, "r");

    if (command_pipe == NULL)
    {
        return -2;
    }

    output[0] = '\0';

    if (fgets(output,
              output_size,
              command_pipe) == NULL)
    {
        pclose(command_pipe);
        return -2;
    }

    pclose(command_pipe);

    output[strcspn(output, "\r\n")] = '\0';

    return 0;
}

/*
 * Validate filename.
 */
int valid_filename(const char *filename)
{
    if (filename == NULL ||
        strlen(filename) == 0)
    {
        return 0;
    }

    if (strlen(filename) > 255)
    {
        return 0;
    }

    if (strstr(filename, "..") != NULL)
    {
        return 0;
    }

    if (strchr(filename, '/') != NULL)
    {
        return 0;
    }

    if (strchr(filename, '\\') != NULL)
    {
        return 0;
    }

    return 1;
}

/*
 * Receive exactly file_size bytes.
 */
int receive_file(int client_socket,
                 unsigned char *input_buffer,
                 size_t *input_length,
                 FILE *file,
                 size_t file_size)
{
    size_t received_total = 0;

    while (received_total < file_size)
    {
        if (*input_length > 0)
        {
            size_t available = *input_length;

            size_t needed =
                file_size - received_total;

            size_t to_write =
                available < needed
                    ? available
                    : needed;

            if (fwrite(input_buffer,
                       1,
                       to_write,
                       file) != to_write)
            {
                return -1;
            }

            received_total += to_write;

            size_t remaining =
                *input_length - to_write;

            memmove(input_buffer,
                    input_buffer + to_write,
                    remaining);

            *input_length = remaining;

            continue;
        }

        unsigned char recv_buffer[4096];

        size_t needed =
            file_size - received_total;

        size_t recv_size =
            needed < sizeof(recv_buffer)
                ? needed
                : sizeof(recv_buffer);

        ssize_t bytes_received =
            recv(client_socket,
                 recv_buffer,
                 recv_size,
                 0);

        if (bytes_received <= 0)
        {
            return -1;
        }

        if (fwrite(recv_buffer,
                   1,
                   bytes_received,
                   file) !=
            (size_t)bytes_received)
        {
            return -1;
        }

        received_total += bytes_received;
    }

    return 0;
}

/*
 * PUT command.
 */
int process_put(ClientSession *session,
                char *command,
                unsigned char *input_buffer,
                size_t *input_length)
{
    char filename[256];
    unsigned long long file_size;

    if (sscanf(command + 4,
               "%255s %llu",
               filename,
               &file_size) != 2)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 004 INVALID_PUT SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    if (!valid_filename(filename))
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 004 INVALID_FILENAME SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    if (file_size > MAX_FILE_SIZE)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 004 FILE_TOO_LARGE SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    char path[512];

    snprintf(path,
             sizeof(path),
             "%s/%s",
             STORAGE_PATH,
             filename);

    FILE *file = fopen(path, "wb");

    if (file == NULL)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 004 FILE_SAVE_FAILED SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    printf("Receiving file: %s (%llu bytes)\n",
           filename,
           file_size);

    int result =
        receive_file(session->client_socket,
                     input_buffer,
                     input_length,
                     file,
                     (size_t)file_size);

    fclose(file);

    if (result != 0)
    {
        remove(path);

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 004 FILE_RECEIVE_FAILED SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return -1;
    }

    printf("File received successfully: %s\n",
           filename);

    char log_message[512];
    snprintf(log_message,
             sizeof(log_message),
             "PUT completed: %s (%llu bytes)",
             filename,
             file_size);
    log_event(log_message);

    char response[512];

    snprintf(response,
             sizeof(response),
             "OK FILE_RECEIVED %s SID:%s\n",
             filename,
             SID);

    send_response(session->client_socket,
                  response);

    return 0;
}

/*
 * GET command.
 */
int process_get(ClientSession *session,
                char *command)
{
    char filename[256];

    if (sscanf(command + 4,
               "%255s",
               filename) != 1)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 005 FILE_NOT_FOUND SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    if (!valid_filename(filename))
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 005 FILE_NOT_FOUND SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    char path[512];

    snprintf(path,
             sizeof(path),
             "%s/%s",
             STORAGE_PATH,
             filename);

    FILE *file = fopen(path, "rb");

    if (file == NULL)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 005 FILE_NOT_FOUND SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    fseek(file, 0, SEEK_END);

    long file_size = ftell(file);

    fseek(file, 0, SEEK_SET);

    if (file_size < 0 ||
        file_size > MAX_FILE_SIZE)
    {
        fclose(file);

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 005 FILE_NOT_FOUND SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    char header[512];

    snprintf(header,
             sizeof(header),
             "OK FILE_SEND %s %ld SID:%s\n",
             filename,
             file_size,
             SID);

    if (send_response(session->client_socket,
                      header) < 0)
    {
        fclose(file);
        return -1;
    }

    printf("Sending file: %s (%ld bytes)\n",
           filename,
           file_size);

    unsigned char buffer[4096];

    long total_sent = 0;

    while (total_sent < file_size)
    {
        long remaining =
            file_size - total_sent;

        size_t to_read =
            remaining < (long)sizeof(buffer)
                ? (size_t)remaining
                : sizeof(buffer);

        size_t bytes_read =
            fread(buffer,
                  1,
                  to_read,
                  file);

        if (bytes_read == 0)
        {
            fclose(file);
            return -1;
        }

        if (send_all(session->client_socket,
                     buffer,
                     bytes_read) < 0)
        {
            fclose(file);
            return -1;
        }

        total_sent += bytes_read;
    }

    fclose(file);

    printf("File sent successfully: %s\n",
           filename);

    char log_message[512];
    snprintf(log_message,
             sizeof(log_message),
             "GET completed: %s (%ld bytes)",
             filename,
             file_size);
    log_event(log_message);

    return 0;
}

/*
 * UDP monitoring thread.
 */
void *monitor_thread_function(void *argument)
{
    ClientSession *session =
        (ClientSession *)argument;

    int udp_socket =
        socket(AF_INET,
               SOCK_DGRAM,
               0);

    if (udp_socket < 0)
    {
        perror("monitor UDP socket");

        pthread_mutex_lock(&session->monitor_mutex);
        session->monitor_active = 0;
        pthread_mutex_unlock(&session->monitor_mutex);

        return NULL;
    }

    struct sockaddr_in destination;

    memset(&destination,
           0,
           sizeof(destination));

    destination.sin_family = AF_INET;

    destination.sin_addr =
        session->client_address.sin_addr;

    destination.sin_port =
        htons(session->monitor_udp_port);

    printf("UDP monitoring started on port %d.\n",
           session->monitor_udp_port);

    char log_message[256];
    snprintf(log_message,
             sizeof(log_message),
             "UDP monitoring started on port %d",
             session->monitor_udp_port);
    log_event(log_message);

    while (1)
    {
        pthread_mutex_lock(&session->monitor_mutex);

        int active =
            session->monitor_active;

        pthread_mutex_unlock(&session->monitor_mutex);

        if (!active)
        {
            break;
        }

        double cpu_load;
        unsigned long memory_used_mb;
        unsigned long uptime_sec;

        get_system_info(&cpu_load,
                        &memory_used_mb,
                        &uptime_sec);

        char packet[512];

        snprintf(packet,
                 sizeof(packet),
                 "SYSINFO %.2f %lu %lu SID:%s\n",
                 cpu_load,
                 memory_used_mb,
                 uptime_sec,
                 SID);

        sendto(udp_socket,
               packet,
               strlen(packet),
               0,
               (struct sockaddr *)&destination,
               sizeof(destination));

        sleep(MONITOR_INTERVAL_SEC);
    }

    close(udp_socket);

    printf("UDP monitoring stopped.\n");

    log_event("UDP monitoring thread stopped");

    return NULL;
}

/*
 * Start UDP monitoring.
 */
int start_monitor(ClientSession *session,
                  int udp_port)
{
    pthread_mutex_lock(&session->monitor_mutex);

    if (session->monitor_active)
    {
        pthread_mutex_unlock(&session->monitor_mutex);

        return -1;
    }

    session->monitor_udp_port = udp_port;
    session->monitor_active = 1;

    pthread_mutex_unlock(&session->monitor_mutex);

    if (pthread_create(&session->monitor_thread,
                       NULL,
                       monitor_thread_function,
                       session) != 0)
    {
        pthread_mutex_lock(&session->monitor_mutex);
        session->monitor_active = 0;
        pthread_mutex_unlock(&session->monitor_mutex);

        return -1;
    }

    return 0;
}

/*
 * Stop UDP monitoring.
 */
int stop_monitor(ClientSession *session)
{
    pthread_mutex_lock(&session->monitor_mutex);

    if (!session->monitor_active)
    {
        pthread_mutex_unlock(&session->monitor_mutex);

        return -1;
    }

    session->monitor_active = 0;

    pthread_mutex_unlock(&session->monitor_mutex);

    pthread_join(session->monitor_thread,
                 NULL);

    return 0;
}

/*
 * Process command.
 */
int process_command(ClientSession *session,
                    char *command,
                    unsigned char *input_buffer,
                    size_t *input_length)
{
    size_t length = strlen(command);

    if (length > 0 &&
        command[length - 1] == '\r')
    {
        command[length - 1] = '\0';
    }

    printf("Received: %s\n", command);

    char log_message[512];

    if (strncmp(command, "AUTH ", 5) == 0)
    {
        snprintf(log_message,
                 sizeof(log_message),
                 "Command: AUTH");
    }
    else
    {
        snprintf(log_message,
                 sizeof(log_message),
                 "Command: %s",
                 command);
    }

    log_event(log_message);

    /*
     * Authentication.
     */
    if (!session->authenticated)
    {
        if (strncmp(command, "AUTH ", 5) == 0)
        {
            char *token = command + 5;

            if (strcmp(token, AUTH_TOKEN) == 0)
            {
                session->authenticated = 1;

                printf("Authentication successful.\n");
                log_event("AUTH successful");

                char response[128];

                snprintf(response,
                         sizeof(response),
                         "OK AUTHENTICATED SID:%s\n",
                         SID);

                send_response(session->client_socket,
                              response);
            }
            else
            {
                char response[128];

                snprintf(response,
                         sizeof(response),
                         "ERR 001 AUTH_FAILED SID:%s\n",
                         SID);

                log_event("AUTH failed");

                send_response(session->client_socket,
                              response);
            }

            return 0;
        }

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 002 AUTH_REQUIRED SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    /*
     * QUIT.
     */
    if (strcmp(command, "QUIT") == 0)
    {
        /*
         * Stop monitoring first.
         */
        pthread_mutex_lock(&session->monitor_mutex);

        int monitor_active =
            session->monitor_active;

        pthread_mutex_unlock(&session->monitor_mutex);

        if (monitor_active)
        {
            stop_monitor(session);
        }

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "OK BYE SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        printf("Controller requested disconnect.\n");
        log_event("Controller requested QUIT");

        return 1;
    }

    /*
     * SYSINFO.
     */
    if (strcmp(command, "SYSINFO") == 0)
    {
        double cpu_load;
        unsigned long memory_used_mb;
        unsigned long uptime_sec;

        get_system_info(&cpu_load,
                        &memory_used_mb,
                        &uptime_sec);

        char response[256];

        snprintf(response,
                 sizeof(response),
                 "OK SYSINFO %.2f %lu %lu SID:%s\n",
                 cpu_load,
                 memory_used_mb,
                 uptime_sec,
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    /*
     * LISTPROC.
     */
    if (strcmp(command, "LISTPROC") == 0)
    {
        char process_list[3000];

        if (get_process_list(process_list,
                             sizeof(process_list)) != 0)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 003 PROCESS_LIST_FAILED SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "OK PROCS %s SID:%s\n",
                 process_list,
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    /*
     * EXEC.
     */
    if (strncmp(command, "EXEC ", 5) == 0)
    {
        char *command_name = command + 5;

        char output[512];

        int result =
            execute_allowed_command(command_name,
                                    output,
                                    sizeof(output));

        if (result == -1)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 002 COMMAND_NOT_ALLOWED SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        if (result == -2)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 006 EXEC_FAILED SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "OK EXEC_RESULT %s SID:%s\n",
                 output,
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    /*
     * PUT.
     */
    if (strncmp(command, "PUT ", 4) == 0)
    {
        return process_put(session,
                           command,
                           input_buffer,
                           input_length);
    }

    /*
     * GET.
     */
    if (strncmp(command, "GET ", 4) == 0)
    {
        return process_get(session,
                           command);
    }

    /*
     * MONITOR START.
     */
    if (strncmp(command, "MONITOR START ", 14) == 0)
    {
        int udp_port;

        if (sscanf(command + 14,
                   "%d",
                   &udp_port) != 1)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 004 INVALID_MONITOR_PORT SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        if (udp_port < 1024 ||
            udp_port > 65535)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 004 INVALID_MONITOR_PORT SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        if (start_monitor(session,
                          udp_port) != 0)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 004 MONITOR_ALREADY_RUNNING SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "OK MONITOR_STARTED %d SID:%s\n",
                 udp_port,
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    /*
     * MONITOR STOP.
     */
    if (strcmp(command, "MONITOR STOP") == 0)
    {
        if (stop_monitor(session) != 0)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 004 MONITOR_NOT_RUNNING SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            return 0;
        }

        char response[128];

        snprintf(response,
                 sizeof(response),
                 "OK MONITOR_STOPPED SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);

        return 0;
    }

    /*
     * Unknown command.
     */
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 003 UNKNOWN_COMMAND SID:%s\n",
                 SID);

        send_response(session->client_socket,
                      response);
    }

    return 0;
}

/*
 * Controller session thread.
 */
void *client_session(void *argument)
{
    ClientSession *session =
        (ClientSession *)argument;

    printf("Controller thread started.\n");

    unsigned char input_buffer[BUFFER_SIZE];

    size_t input_length = 0;

    while (session->connection_open)
    {
        unsigned char recv_buffer[1024];

        ssize_t bytes_received =
            recv(session->client_socket,
                 recv_buffer,
                 sizeof(recv_buffer),
                 0);

        if (bytes_received < 0)
        {
            perror("recv");
            break;
        }

        if (bytes_received == 0)
        {
            printf("Controller disconnected.\n");
            log_event("Controller disconnected");
            break;
        }

        if (input_length + bytes_received >= BUFFER_SIZE)
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 005 INPUT_TOO_LARGE SID:%s\n",
                     SID);

            send_response(session->client_socket,
                          response);

            input_length = 0;

            continue;
        }

        memcpy(input_buffer + input_length,
               recv_buffer,
               bytes_received);

        input_length += bytes_received;

        /*
         * Process every complete line.
         */
        while (session->connection_open)
        {
            unsigned char *newline =
                memchr(input_buffer,
                       '\n',
                       input_length);

            if (newline == NULL)
            {
                break;
            }

            size_t command_length =
                newline - input_buffer;

            char command[BUFFER_SIZE];

            memcpy(command,
                   input_buffer,
                   command_length);

            command[command_length] = '\0';

            size_t remaining =
                input_length -
                (command_length + 1);

            memmove(input_buffer,
                    newline + 1,
                    remaining);

            input_length = remaining;

            int result =
                process_command(session,
                                command,
                                input_buffer,
                                &input_length);

            if (result == 1)
            {
                session->connection_open = 0;
                break;
            }
        }
    }

    /*
     * Stop monitoring if Controller disconnects
     * without sending QUIT.
     */
    pthread_mutex_lock(&session->monitor_mutex);

    int monitor_active =
        session->monitor_active;

    pthread_mutex_unlock(&session->monitor_mutex);

    if (monitor_active)
    {
        stop_monitor(session);
    }

    close(session->client_socket);

    pthread_mutex_destroy(&session->monitor_mutex);

    printf("Controller thread finished.\n");
    log_event("Controller thread finished");

    free(session);

    return NULL;
}

/*
 * Main Agent.
 */
int main(void)
{
    int server_socket;

    struct sockaddr_in server_address;

    mkdir("agentfiles", 0755);
    mkdir(STORAGE_PATH, 0755);

    server_socket =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (server_socket < 0)
    {
        perror("socket");
        return 1;
    }

    int option = 1;

    if (setsockopt(server_socket,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &option,
                   sizeof(option)) < 0)
    {
        perror("setsockopt");
        close(server_socket);
        return 1;
    }

    memset(&server_address,
           0,
           sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    if (bind(server_socket,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
        perror("bind");
        close(server_socket);
        return 1;
    }

    if (listen(server_socket, 10) < 0)
    {
        perror("listen");
        close(server_socket);
        return 1;
    }

    printf("RemoteOps Agent listening on TCP port %d...\n",
           PORT);

    printf("Multi-client mode enabled.\n");

    while (1)
    {
        struct sockaddr_in client_address;

        socklen_t client_length =
            sizeof(client_address);

        ClientSession *session =
            malloc(sizeof(ClientSession));

        if (session == NULL)
        {
            perror("malloc");
            continue;
        }

        memset(session,
               0,
               sizeof(ClientSession));

        session->client_socket =
            accept(server_socket,
                   (struct sockaddr *)&client_address,
                   &client_length);

        if (session->client_socket < 0)
        {
            perror("accept");
            free(session);
            continue;
        }

        session->client_address =
            client_address;

        session->authenticated = 0;
        session->connection_open = 1;
        session->monitor_active = 0;

        pthread_mutex_init(&session->monitor_mutex,
                           NULL);

        printf("Controller connected.\n");
        log_event("Controller connected");

        pthread_t thread_id;

        if (pthread_create(&thread_id,
                           NULL,
                           client_session,
                           session) != 0)
        {
            perror("pthread_create");

            close(session->client_socket);

            pthread_mutex_destroy(
                &session->monitor_mutex);

            free(session);

            continue;
        }

        pthread_detach(thread_id);

        printf("Client thread created.\n");
    }

    close(server_socket);

    return 0;
}

