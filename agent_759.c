#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410
#define BUFFER_SIZE 1024
#define AUTH_TOKEN "OPS-2759"
#define SID "9572"

int main(void)
{
    int server_fd;
    int client_fd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    socklen_t client_len = sizeof(client_addr);

    char buffer[BUFFER_SIZE];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("RemoteOps Agent listening on TCP port %d...\n", PORT);

    while (1)
    {
        printf("Waiting for Controller connection...\n");

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_len);

        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        printf("Controller connected.\n");

        int authenticated = 0;

        /*
         * Wait for the first command.
         * The first command must be AUTH.
         */
        memset(buffer, 0, sizeof(buffer));

        ssize_t bytes_received =
            recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received <= 0)
        {
            close(client_fd);
            continue;
        }

        buffer[bytes_received] = '\0';

        printf("Received: %s", buffer);

        /*
         * Check AUTH command.
         */
        char received_token[BUFFER_SIZE];

        if (sscanf(buffer, "AUTH %1023s", received_token) == 1)
        {
            if (strcmp(received_token, AUTH_TOKEN) == 0)
            {
                authenticated = 1;

                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "OK AUTHENTICATED SID:%s\n",
                         SID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                printf("Authentication successful.\n");
            }
            else
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR 001 AUTH_FAILED SID:%s\n",
                         SID);

                send(client_fd,
                     response,
                     strlen(response),
                     0);

                printf("Authentication failed.\n");
            }
        }
        else
        {
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "ERR 001 AUTH_FAILED SID:%s\n",
                     SID);

            send(client_fd,
                 response,
                 strlen(response),
                 0);

            printf("First command was not AUTH.\n");
        }

        /*
         * For now, close the connection after authentication test.
         * Later we will keep this connection open and process
         * SYSINFO, LISTPROC, EXEC, PUT, GET, MONITOR and QUIT.
         */
        if (authenticated)
        {
            printf("Controller authenticated.\n");
        }

        close(client_fd);

        printf("Controller disconnected.\n");
    }

    close(server_fd);

    return 0;
}
