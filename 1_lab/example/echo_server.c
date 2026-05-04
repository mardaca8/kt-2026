/*
 * Echo serveris
 * 
 * Author: Kæstutis Mizara
 * Description: Gauna kliento praneðimà ir iðsiunèia atgal
 */

#ifdef _WIN32
#include <winsock2.h>
#define socklen_t int
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv []){
#ifdef _WIN32
    WSADATA data;
#endif
    unsigned int port;
    int l_socket; // socket'as skirtas prisijungimø laukimui
    int c_socket; // prisijungusio kliento socket'as

    struct sockaddr_in servaddr; // Serverio adreso struktûra
    struct sockaddr_in clientaddr; // Prisijungusio kliento adreso struktûra
//    int clientaddrlen;
    socklen_t clientaddrlen;

    int s_len;
    int r_len;
    char buffer[1024];
    
    if (argc != 2){
        printf("USAGE: %s <port>\n", argv[0]);
        exit(1);
    }

    port = atoi(argv[1]);

    if ((port < 1) || (port > 65535)){
        printf("ERROR #1: invalid port specified.\n");
        exit(1);
    }

#ifdef _WIN32
    WSAStartup(MAKEWORD(2,2),&data);    
#endif

    /*
     * Sukuriamas serverio socket'as
     */
    if ((l_socket = socket(AF_INET, SOCK_STREAM,0))< 0){
        fprintf(stderr,"ERROR #2: cannot create listening socket.\n");
        exit(1);
    }
    
    /*
     * Iðvaloma ir uþpildoma serverio adreso struktûra
     */
    memset(&servaddr,0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; // nurodomas protokolas (IP)

    /*
     * Nurodomas IP adresas, kuriuo bus laukiama klientø, ðiuo atveju visi 
     * esami sistemos IP adresai (visi interfeis'ai)
     */
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    servaddr.sin_port = htons(port); // nurodomas portas
    
    /*
     * Serverio adresas susiejamas su socket'u
     */
    if (bind (l_socket, (struct sockaddr *)&servaddr,sizeof(servaddr))<0){
        fprintf(stderr,"ERROR #3: bind listening socket.\n");
        exit(1);
    }

    /*
     * Nurodoma, kad socket'u l_socket bus laukiama klientø prisijungimo,
     * eilëje ne daugiau kaip 5 aptarnavimo laukiantys klientai
     */
    if (listen(l_socket, 5) <0){
        fprintf(stderr,"ERROR #4: error in listen().\n");
        exit(1);
    }

    for(;;){
        /*
         * Iðvalomas buferis ir kliento adreso struktûra
         */
        memset(&clientaddr,0, sizeof(clientaddr));
        memset(&buffer,0,sizeof(buffer));

        /*
         * Laukiama klientø prisijungimø
         */
        clientaddrlen = sizeof(struct sockaddr);
        if ((c_socket = accept(l_socket,
            (struct sockaddr*)&clientaddr,&clientaddrlen))<0){
            fprintf(stderr,"ERROR #5: error occured accepting connection.\n");
            exit(1);
        }

        /*
         * Sulaukus kliento prisijungimo skaitomi kliento siunèiami duomenys
         */
        s_len = recv(c_socket,buffer,sizeof(buffer),0);

        /*
         * Klientui iðsiunèiami gauti duomenys
         */
        r_len = send(c_socket,buffer,s_len,0);


        printf("IP: %s Sent: %d Received: %d\n",inet_ntoa(clientaddr.sin_addr),
            s_len, r_len
        );
        
        /*
         * Klientas atjungiamas
         */
        close(c_socket);
    }

    return 0;
}
