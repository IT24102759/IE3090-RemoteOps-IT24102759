#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define SERVER_PORT 9410
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 4096
#define AUTH_TOKEN "OPS-2759"

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
            return -1;

        total_sent += sent;
    }

    return 0;
}

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
            return -1;

        buffer[received++] = character;

        if (character == '\n')
            break;
    }

    buffer[received] = '\0';

    return 0;
}

int upload_file(int socket_fd,
                const char *filename)
{
    FILE *file = fopen(filename, "rb");

    if (file == NULL)
    {
        perror("fopen");
        return 0;
    }

    fseek(file, 0, SEEK_END);

    long file_size = ftell(file);

    fseek(file, 0, SEEK_SET);

    if (file_size < 0)
    {
        fclose(file);
        return 0;
    }

    /*
     * Build PUT header.
     */
    char header[512];

    snprintf(header,
             sizeof(header),
             "PUT %s %ld\n",
             filename,
             file_size);

    printf("Controller: PUT %s (%ld bytes)\n",
           filename,
           file_size);

    /*
     * Send header.
     */
    if (send_all(socket_fd,
                 (unsigned char *)header,
                 strlen(header)) < 0)
    {
        fclose(file);
        return 0;
    }

    /*
     * Send exact file bytes.
     */
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

int main(void)
{
    int socket_fd;

    struct sockaddr_in server_address;

    socket_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (socket_fd < 0)
    {
        perror("socket");
        return 1;
    }

    memset(&server_address,
           0,
           sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET,
                  SERVER_IP,
                  &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(socket_fd);
        return 1;
    }

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
     * Authenticate.
     */
    char auth_command[128];

    snprintf(auth_command,
             sizeof(auth_command),
             "AUTH %s\n",
             AUTH_TOKEN);

    printf("Controller: AUTH %s\n",
           AUTH_TOKEN);

    send_all(socket_fd,
             (unsigned char *)auth_command,
             strlen(auth_command));

    char response[BUFFER_SIZE];

    if (receive_response(socket_fd,
                         response,
                         sizeof(response)) < 0)
    {
        printf("Agent disconnected.\n");
        close(socket_fd);
        return 1;
    }

    printf("Agent: %s", response);

    /*
     * Interactive commands.
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

        command[strcspn(command, "\n")] = '\0';

        if (strlen(command) == 0)
            continue;

        /*
         * PUT is handled separately because
         * raw file bytes follow the header.
         */
        if (strncmp(command, "PUT ", 4) == 0)
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

            printf("Agent: %s", response);

            continue;
        }

        /*
         * Normal text command.
         */
        char command_to_send[BUFFER_SIZE + 2];

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

        printf("Agent: %s", response);

        if (strcmp(command, "QUIT") == 0)
            break;
    }

    close(socket_fd);

    printf("Disconnected from Agent.\n");

    return 0;
}
