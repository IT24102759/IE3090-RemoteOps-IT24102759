#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define SERVER_PORT 9410
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 4096
#define AUTH_TOKEN "OPS-2759"

/*
 * UDP monitoring receiver information.
 */
typedef struct
{
    int udp_socket;
    int running;
    pthread_t thread;

} MonitorReceiver;


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
 * Receive one complete newline-terminated response.
 */
int receive_response(int socket_fd,
                     char *buffer,
                     size_t buffer_size)
{
    size_t received = 0;

    while (received < buffer_size - 1)
    {
        char character;

        ssize_t bytes =
            recv(socket_fd,
                 &character,
                 1,
                 0);

        if (bytes <= 0)
        {
            return -1;
        }

        buffer[received++] = character;

        if (character == '\n')
        {
            break;
        }
    }

    buffer[received] = '\0';

    return 0;
}


/*
 * Upload a file using PUT.
 */
int upload_file(int socket_fd,
                const char *filename)
{
    FILE *file =
        fopen(filename, "rb");

    if (file == NULL)
    {
        perror("fopen");
        return 0;
    }

    fseek(file, 0, SEEK_END);

    long file_size =
        ftell(file);

    fseek(file, 0, SEEK_SET);

    if (file_size < 0)
    {
        fclose(file);
        return 0;
    }

    char header[512];

    snprintf(header,
             sizeof(header),
             "PUT %s %ld\n",
             filename,
             file_size);

    printf("Controller: PUT %s (%ld bytes)\n",
           filename,
           file_size);

    if (send_all(socket_fd,
                 (unsigned char *)header,
                 strlen(header)) < 0)
    {
        fclose(file);
        return 0;
    }

    unsigned char buffer[4096];

    size_t total_sent = 0;

    while (total_sent < (size_t)file_size)
    {
        size_t remaining =
            (size_t)file_size - total_sent;

        size_t to_read =
            remaining < sizeof(buffer)
                ? remaining
                : sizeof(buffer);

        size_t bytes_read =
            fread(buffer,
                  1,
                  to_read,
                  file);

        if (bytes_read == 0)
        {
            fclose(file);
            return 0;
        }

        if (send_all(socket_fd,
                     buffer,
                     bytes_read) < 0)
        {
            fclose(file);
            return 0;
        }

        total_sent += bytes_read;
    }

    fclose(file);

    return 1;
}


/*
 * Download a file using GET.
 */
int download_file(int socket_fd,
                  const char *filename)
{
    char command[512];

    snprintf(command,
             sizeof(command),
             "GET %s\n",
             filename);

    printf("Controller: GET %s\n",
           filename);

    if (send_all(socket_fd,
                 (unsigned char *)command,
                 strlen(command)) < 0)
    {
        return 0;
    }

    char response[BUFFER_SIZE];

    if (receive_response(socket_fd,
                         response,
                         sizeof(response)) < 0)
    {
        printf("Agent disconnected.\n");
        return 0;
    }

    printf("Agent: %s",
           response);

    if (strncmp(response,
                "OK FILE_SEND ",
                13) != 0)
    {
        return 0;
    }

    char received_filename[256];

    unsigned long long file_size;

    char received_sid[64];

    if (sscanf(response,
               "OK FILE_SEND %255s %llu SID:%63s",
               received_filename,
               &file_size,
               received_sid) != 3)
    {
        printf("Invalid FILE_SEND response.\n");
        return 0;
    }

    char output_filename[512];

    snprintf(output_filename,
             sizeof(output_filename),
             "downloaded_%s",
             received_filename);

    FILE *file =
        fopen(output_filename, "wb");

    if (file == NULL)
    {
        perror("fopen");
        return 0;
    }

    unsigned char buffer[4096];

    unsigned long long total_received = 0;

    while (total_received < file_size)
    {
        unsigned long long remaining =
            file_size - total_received;

        size_t to_receive =
            remaining < sizeof(buffer)
                ? (size_t)remaining
                : sizeof(buffer);

        ssize_t bytes_received =
            recv(socket_fd,
                 buffer,
                 to_receive,
                 0);

        if (bytes_received <= 0)
        {
            fclose(file);
            remove(output_filename);

            return 0;
        }

        if (fwrite(buffer,
                   1,
                   bytes_received,
                   file) !=
            (size_t)bytes_received)
        {
            fclose(file);
            remove(output_filename);

            return 0;
        }

        total_received += bytes_received;
    }

    fclose(file);

    printf("Downloaded %s (%llu bytes)\n",
           output_filename,
           file_size);

    return 1;
}


/*
 * UDP monitoring receiver thread.
 */
void *monitor_receiver(void *argument)
{
    MonitorReceiver *monitor =
        (MonitorReceiver *)argument;

    char buffer[1024];

    struct sockaddr_in sender_address;

    socklen_t sender_length =
        sizeof(sender_address);

    printf("UDP monitoring receiver started.\n");

    while (monitor->running)
    {
        ssize_t bytes_received =
            recvfrom(monitor->udp_socket,
                     buffer,
                     sizeof(buffer) - 1,
                     0,
                     (struct sockaddr *)&sender_address,
                     &sender_length);

        if (bytes_received < 0)
        {
            if (monitor->running)
            {
                perror("recvfrom");
            }

            break;
        }

        buffer[bytes_received] = '\0';

        if (monitor->running)
        {
            printf("\n[UDP MONITOR] %s",
                   buffer);

            printf("RemoteOps> ");
            fflush(stdout);
        }
    }

    return NULL;
}


/*
 * Start UDP receiver.
 */
int start_udp_monitor(MonitorReceiver *monitor,
                       int udp_port)
{
    monitor->udp_socket =
        socket(AF_INET,
               SOCK_DGRAM,
               0);

    if (monitor->udp_socket < 0)
    {
        perror("UDP socket");
        return -1;
    }

    struct sockaddr_in local_address;

    memset(&local_address,
           0,
           sizeof(local_address));

    local_address.sin_family =
        AF_INET;

    local_address.sin_addr.s_addr =
        htonl(INADDR_ANY);

    local_address.sin_port =
        htons(udp_port);

    if (bind(monitor->udp_socket,
             (struct sockaddr *)&local_address,
             sizeof(local_address)) < 0)
    {
        perror("UDP bind");

        close(monitor->udp_socket);

        return -1;
    }

    monitor->running = 1;

    if (pthread_create(&monitor->thread,
                       NULL,
                       monitor_receiver,
                       monitor) != 0)
    {
        perror("pthread_create");

        monitor->running = 0;

        close(monitor->udp_socket);

        return -1;
    }

    return 0;
}


/*
 * Stop UDP receiver.
 */
void stop_udp_monitor(MonitorReceiver *monitor)
{
    if (!monitor->running)
    {
        return;
    }

    monitor->running = 0;

    /*
     * Shutdown and close the UDP socket.
     */
    shutdown(monitor->udp_socket,
             SHUT_RDWR);

    close(monitor->udp_socket);

    pthread_join(monitor->thread,
                 NULL);

    printf("UDP monitoring receiver stopped.\n");
}


/*
 * Main Controller.
 */
int main(void)
{
    int socket_fd;

    struct sockaddr_in server_address;

    MonitorReceiver monitor;

    memset(&monitor,
           0,
           sizeof(monitor));

    /*
     * Create TCP socket.
     */
    socket_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (socket_fd < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Configure Agent address.
     */
    memset(&server_address,
           0,
           sizeof(server_address));

    server_address.sin_family =
        AF_INET;

    server_address.sin_port =
        htons(SERVER_PORT);

    if (inet_pton(AF_INET,
                  SERVER_IP,
                  &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");

        close(socket_fd);

        return 1;
    }

    /*
     * Connect to Agent.
     */
    if (connect(socket_fd,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0)
    {
        perror("connect");

        close(socket_fd);

        return 1;
    }

    printf("Connected to RemoteOps Agent.\n");

    /*
     * Authentication.
     */
    char auth_command[128];

    snprintf(auth_command,
             sizeof(auth_command),
             "AUTH %s\n",
             AUTH_TOKEN);

    printf("Controller: AUTH %s\n",
           AUTH_TOKEN);

    if (send_all(socket_fd,
                 (unsigned char *)auth_command,
                 strlen(auth_command)) < 0)
    {
        close(socket_fd);
        return 1;
    }

    char response[BUFFER_SIZE];

    if (receive_response(socket_fd,
                         response,
                         sizeof(response)) < 0)
    {
        printf("Agent disconnected.\n");

        close(socket_fd);

        return 1;
    }

    printf("Agent: %s",
           response);

    /*
     * Interactive command loop.
     */
    char command[BUFFER_SIZE];

    while (1)
    {
        printf("RemoteOps> ");
        fflush(stdout);

        if (fgets(command,
                  sizeof(command),
                  stdin) == NULL)
        {
            break;
        }

        command[strcspn(command,
                        "\n")] = '\0';

        if (strlen(command) == 0)
        {
            continue;
        }

        /*
         * PUT.
         */
        if (strncmp(command,
                     "PUT ",
                     4) == 0)
        {
            char filename[256];

            if (sscanf(command + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: PUT <filename>\n");
                continue;
            }

            if (!upload_file(socket_fd,
                             filename))
            {
                printf("File upload failed.\n");
                continue;
            }

            if (receive_response(socket_fd,
                                 response,
                                 sizeof(response)) < 0)
            {
                printf("Agent disconnected.\n");
                break;
            }

            printf("Agent: %s",
                   response);

            continue;
        }

        /*
         * GET.
         */
        if (strncmp(command,
                    "GET ",
                    4) == 0)
        {
            char filename[256];

            if (sscanf(command + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: GET <filename>\n");
                continue;
            }

            if (!download_file(socket_fd,
                               filename))
            {
                printf("File download failed.\n");
            }

            continue;
        }

        /*
         * MONITOR START.
         */
        if (strncmp(command,
                    "MONITOR START ",
                    14) == 0)
        {
            int udp_port;

            if (sscanf(command + 14,
                       "%d",
                       &udp_port) != 1)
            {
                printf("Usage: MONITOR START <udp_port>\n");
                continue;
            }

            if (udp_port < 1024 ||
                udp_port > 65535)
            {
                printf("Invalid UDP port.\n");
                continue;
            }

            /*
             * Start UDP listener BEFORE
             * telling Agent to send packets.
             */
            if (start_udp_monitor(&monitor,
                                  udp_port) != 0)
            {
                printf("Could not start UDP receiver.\n");
                continue;
            }

            printf("Controller UDP port %d ready.\n",
                   udp_port);

            char monitor_command[128];

            snprintf(monitor_command,
                     sizeof(monitor_command),
                     "MONITOR START %d\n",
                     udp_port);

            if (send_all(socket_fd,
                         (unsigned char *)monitor_command,
                         strlen(monitor_command)) < 0)
            {
                stop_udp_monitor(&monitor);

                printf("Failed to send MONITOR START.\n");

                break;
            }

            if (receive_response(socket_fd,
                                 response,
                                 sizeof(response)) < 0)
            {
                stop_udp_monitor(&monitor);

                printf("Agent disconnected.\n");

                break;
            }

            printf("Agent: %s",
                   response);

            /*
             * If Agent rejected the command,
             * stop local UDP receiver.
             */
            if (strncmp(response,
                         "OK MONITOR_STARTED",
                        18) != 0)
            {
                stop_udp_monitor(&monitor);
            }

            continue;
        }

        /*
         * MONITOR STOP.
         */
        if (strcmp(command,
                   "MONITOR STOP") == 0)
        {
            char monitor_command[] =
                "MONITOR STOP\n";

            if (send_all(socket_fd,
                         (unsigned char *)monitor_command,
                         strlen(monitor_command)) < 0)
            {
                printf("Failed to send MONITOR STOP.\n");
                break;
            }

            if (receive_response(socket_fd,
                                 response,
                                 sizeof(response)) < 0)
            {
                printf("Agent disconnected.\n");
                break;
            }

            printf("Agent: %s",
                   response);

            if (strncmp(response,
                        "OK MONITOR_STOPPED",
                        18) == 0)
            {
                stop_udp_monitor(&monitor);
            }

            continue;
        }

        /*
         * Normal TCP command.
         */
        char command_to_send[
            BUFFER_SIZE + 2
        ];

        snprintf(command_to_send,
                 sizeof(command_to_send),
                 "%s\n",
                 command);

        printf("Controller: %s\n",
               command);

        if (send_all(socket_fd,
                     (unsigned char *)command_to_send,
                     strlen(command_to_send)) < 0)
        {
            perror("send");
            break;
        }

        if (receive_response(socket_fd,
                             response,
                             sizeof(response)) < 0)
        {
            printf("Agent disconnected.\n");
            break;
        }

        printf("Agent: %s",
               response);

        if (strcmp(command,
                   "QUIT") == 0)
        {
            break;
        }
    }

    /*
     * Stop UDP monitoring if still active.
     */
    if (monitor.running)
    {
        stop_udp_monitor(&monitor);
    }

    close(socket_fd);

    printf("Disconnected from Agent.\n");

    return 0;
}
