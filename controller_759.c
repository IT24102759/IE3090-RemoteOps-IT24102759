#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SERVER_PORT 9410
#define SERVER_IP "127.0.0.1"

#define BUFFER_SIZE 4096
#define AUTH_TOKEN "OPS-2759"

/*
 * Send the complete command
 */
int send_command(int socket_fd, const char *command)
{
    size_t total_sent = 0;
    size_t length = strlen(command);

    while (total_sent < length)
    {
        ssize_t sent = send(socket_fd,
                            command + total_sent,
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
 * Receive one response line.
 *
 * TCP may deliver a response in multiple pieces,
 * so continue receiving until '\n' is found.
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

int main(void)
{
    int socket_fd;

    struct sockaddr_in server_address;

    /*
     * Create TCP socket
     */
    socket_fd = socket(AF_INET,
                       SOCK_STREAM,
                       0);

    if (socket_fd < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Configure server address
     */
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

    /*
     * Connect to Agent
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
     * Authentication
     */
    char auth_command[128];

    snprintf(auth_command,
             sizeof(auth_command),
             "AUTH %s\n",
             AUTH_TOKEN);

    printf("Controller: AUTH %s\n", AUTH_TOKEN);

    if (send_command(socket_fd, auth_command) < 0)
    {
        perror("send");
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

    printf("Agent: %s", response);

    /*
     * Interactive command loop
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

        /*
         * Remove newline from user input
         */
        command[strcspn(command, "\n")] = '\0';

        /*
         * Ignore empty commands
         */
        if (strlen(command) == 0)
        {
            continue;
        }

        /*
         * Add protocol newline
         */
        char command_to_send[BUFFER_SIZE + 2];

        snprintf(command_to_send,
                 sizeof(command_to_send),
                 "%s\n",
                 command);

        printf("Controller: %s\n", command);

        if (send_command(socket_fd,
                         command_to_send) < 0)
        {
            perror("send");
            break;
        }

        /*
         * Receive Agent response
         */
        if (receive_response(socket_fd,
                             response,
                             sizeof(response)) < 0)
        {
            printf("Agent disconnected.\n");
            break;
        }

        printf("Agent: %s", response);

        /*
         * QUIT closes the session.
         */
        if (strcmp(command, "QUIT") == 0)
        {
            break;
        }
    }

    close(socket_fd);

    printf("Disconnected from Agent.\n");

    return 0;
}
