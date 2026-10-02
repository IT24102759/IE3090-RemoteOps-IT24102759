#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SERVER_PORT 9410
#define BUFFER_SIZE 1024
#define AUTH_TOKEN "OPS-2759"

int main(void)
{
    int sockfd;

    struct sockaddr_in server_addr;

    char buffer[BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET,
                  "127.0.0.1",
                  &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(sockfd);
        return 1;
    }

    if (connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sockfd);
        return 1;
    }

    printf("Connected to RemoteOps Agent.\n");

    /*
     * Send authentication command.
     */
    char auth_command[BUFFER_SIZE];

    snprintf(auth_command,
             sizeof(auth_command),
             "AUTH %s\n",
             AUTH_TOKEN);

    send(sockfd,
         auth_command,
         strlen(auth_command),
         0);

    printf("Controller sent: %s", auth_command);

    /*
     * Receive authentication response.
     */
    memset(buffer, 0, sizeof(buffer));

    ssize_t bytes_received =
        recv(sockfd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received > 0)
    {
        buffer[bytes_received] = '\0';

        printf("Agent: %s", buffer);
    }

    close(sockfd);

    printf("Disconnected from Agent.\n");

    return 0;
}
