#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410
#define BUFFER_SIZE 4096
#define AUTH_TOKEN "OPS-2759"
#define SID "9572"

/*
 * Send the complete response
 */
int send_response(int client_socket, const char *response)
{
    size_t total_sent = 0;
    size_t length = strlen(response);

    while (total_sent < length)
    {
        ssize_t sent = send(client_socket,
                            response + total_sent,
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
 * Get current system information from Linux /proc
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
     * CPU load
     */
    file = fopen("/proc/loadavg", "r");

    if (file != NULL)
    {
        fscanf(file, "%lf", cpu_load);
        fclose(file);
    }

    /*
     * Memory usage
     */
    unsigned long mem_total_kb = 0;
    unsigned long mem_available_kb = 0;

    file = fopen("/proc/meminfo", "r");

    if (file != NULL)
    {
        char line[256];

        while (fgets(line, sizeof(line), file) != NULL)
        {
            if (sscanf(line,
                       "MemTotal: %lu kB",
                       &mem_total_kb) == 1)
            {
                continue;
            }

            if (sscanf(line,
                       "MemAvailable: %lu kB",
                       &mem_available_kb) == 1)
            {
                continue;
            }
        }

        fclose(file);
    }

    if (mem_total_kb >= mem_available_kb)
    {
        *memory_used_mb =
            (mem_total_kb - mem_available_kb) / 1024;
    }

    /*
     * System uptime
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
 * Get currently running processes.
 *
 * The assignment allows using ps for a simple
 * process snapshot.
 *
 * Output example:
 *
 * 1/systemd,2/kthreadd,3/rcu_gp,...
 */
int get_process_list(char *process_list,
                     size_t buffer_size)
{
    FILE *process_pipe;

    process_pipe =
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

        /*
         * Remove trailing newline if present
         */
        process_name[strcspn(process_name, "\r\n")] = '\0';

        /*
         * Format:
         * PID/process_name
         */
        char entry[160];

        snprintf(entry,
                 sizeof(entry),
                 "%d/%s",
                 pid,
                 process_name);

        size_t entry_length = strlen(entry);

        /*
         * Leave room for:
         * comma
         * SID
         * newline
         * null terminator
         */
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
 * Process one complete command
 */
int process_command(int client_socket,
                    char *command,
                    int *authenticated)
{
    /*
     * Remove trailing CR if command uses CRLF
     */
    size_t length = strlen(command);

    if (length > 0 && command[length - 1] == '\r')
    {
        command[length - 1] = '\0';
    }

    printf("Received: %s\n", command);

    /*
     * Authentication must happen first.
     */
    if (!(*authenticated))
    {
        if (strncmp(command, "AUTH ", 5) == 0)
        {
            char *token = command + 5;

            if (strcmp(token, AUTH_TOKEN) == 0)
            {
                *authenticated = 1;

                printf("Authentication successful.\n");

                char response[128];

                snprintf(response,
                         sizeof(response),
                         "OK AUTHENTICATED SID:%s\n",
                         SID);

                send_response(client_socket, response);

                return 0;
            }
            else
            {
                char response[128];

                snprintf(response,
                         sizeof(response),
                         "ERR 001 AUTH_FAILED SID:%s\n",
                         SID);

                send_response(client_socket, response);

                return 0;
            }
        }
        else
        {
            char response[128];

            snprintf(response,
                     sizeof(response),
                     "ERR 002 AUTH_REQUIRED SID:%s\n",
                     SID);

            send_response(client_socket, response);

            return 0;
        }
    }

    /*
     * QUIT
     */
    if (strcmp(command, "QUIT") == 0)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "OK BYE SID:%s\n",
                 SID);

        send_response(client_socket, response);

        printf("Controller requested disconnect.\n");

        return 1;
    }

    /*
     * SYSINFO
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

        send_response(client_socket, response);

        return 0;
    }

    /*
     * LISTPROC
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

            send_response(client_socket, response);

            return 0;
        }

        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "OK PROCS %s SID:%s\n",
                 process_list,
                 SID);

        send_response(client_socket, response);

        return 0;
    }

    /*
     * Commands that will be implemented
     * in later commits.
     */
    if (strncmp(command, "EXEC ", 5) == 0 ||
        strncmp(command, "PUT ", 4) == 0 ||
        strncmp(command, "GET ", 4) == 0 ||
        strcmp(command, "MONITOR START") == 0 ||
        strcmp(command, "MONITOR STOP") == 0)
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 004 NOT_IMPLEMENTED SID:%s\n",
                 SID);

        send_response(client_socket, response);

        return 0;
    }

    /*
     * Unknown command
     */
    {
        char response[128];

        snprintf(response,
                 sizeof(response),
                 "ERR 003 UNKNOWN_COMMAND SID:%s\n",
                 SID);

        send_response(client_socket, response);
    }

    return 0;
}

int main(void)
{
    int server_socket;
    int client_socket;

    struct sockaddr_in server_address;
    struct sockaddr_in client_address;

    socklen_t client_length = sizeof(client_address);

    /*
     * Create TCP socket
     */
    server_socket = socket(AF_INET,
                           SOCK_STREAM,
                           0);

    if (server_socket < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Allow port reuse
     */
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

    /*
     * Configure server address
     */
    memset(&server_address,
           0,
           sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    /*
     * Bind socket
     */
    if (bind(server_socket,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
        perror("bind");
        close(server_socket);
        return 1;
    }

    /*
     * Start listening
     */
    if (listen(server_socket, 5) < 0)
    {
        perror("listen");
        close(server_socket);
        return 1;
    }

    printf("RemoteOps Agent listening on TCP port %d...\n", PORT);

    /*
     * Accept Controllers continuously
     */
    while (1)
    {
        printf("Waiting for Controller connection...\n");

        client_socket = accept(server_socket,
                               (struct sockaddr *)&client_address,
                               &client_length);

        if (client_socket < 0)
        {
            perror("accept");
            continue;
        }

        printf("Controller connected.\n");

        int authenticated = 0;
        int connection_open = 1;

        /*
         * TCP input buffer
         */
        char input_buffer[BUFFER_SIZE];

        size_t input_length = 0;

        while (connection_open)
        {
            char recv_buffer[1024];

            ssize_t bytes_received =
                recv(client_socket,
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
                break;
            }

            /*
             * Prevent buffer overflow
             */
            if (input_length + bytes_received >= BUFFER_SIZE)
            {
                char response[128];

                snprintf(response,
                         sizeof(response),
                         "ERR 005 INPUT_TOO_LARGE SID:%s\n",
                         SID);

                send_response(client_socket, response);

                input_length = 0;

                continue;
            }

            /*
             * Add received data to input buffer
             */
            memcpy(input_buffer + input_length,
                   recv_buffer,
                   bytes_received);

            input_length += bytes_received;

            input_buffer[input_length] = '\0';

            /*
             * Process every complete line
             */
            while (1)
            {
                char *newline_position =
                    memchr(input_buffer,
                           '\n',
                           input_length);

                if (newline_position == NULL)
                {
                    break;
                }

                size_t command_length =
                    newline_position - input_buffer;

                char command[BUFFER_SIZE];

                memcpy(command,
                       input_buffer,
                       command_length);

                command[command_length] = '\0';

                /*
                 * Remove processed command
                 * from input buffer.
                 */
                size_t remaining =
                    input_length -
                    (command_length + 1);

                memmove(input_buffer,
                        newline_position + 1,
                        remaining);

                input_length = remaining;

                input_buffer[input_length] = '\0';

                /*
                 * Process command
                 */
                int result =
                    process_command(client_socket,
                                    command,
                                    &authenticated);

                if (result == 1)
                {
                    connection_open = 0;
                    break;
                }
            }
        }

        close(client_socket);

        printf("Controller session closed.\n");
    }

    close(server_socket);

    return 0;
}
