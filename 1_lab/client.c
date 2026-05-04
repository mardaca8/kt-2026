/*
 * Echo klientas
 * 
 * Author: K�stutis Mizara
 * Description: I�siun�ia serveriui prane�im� ir j� gauna
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFLEN 1024

int main(int argc, char *argv[]){
    unsigned int port;
    int s_socket;
    struct sockaddr_in servaddr; // Serverio adreso strukt�ra

    char buffer[BUFFLEN];

    if (argc != 3){
        fprintf(stderr,"USAGE: %s <ip> <port>\n",argv[0]);
        exit(1);
    }

    port = atoi(argv[2]);

    if ((port < 1) || (port > 65535)){
        printf("ERROR #1: invalid port specified.\n");
        exit(1);
    }

    /*
     * Sukuriamas socket'as
     */
    if ((s_socket = socket(AF_INET, SOCK_STREAM,0))< 0){
        fprintf(stderr,"ERROR #2: cannot create socket.\n");
        exit(1);
    }
                                
   /*
    * I�valoma ir u�pildoma serverio strukt�ra
    */
    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET; // nurodomas protokolas (IP)
    servaddr.sin_port = htons(port); // nurodomas portas
    
    /*
     * I�ver�iamas simboli� eilut�je u�ra�ytas ip � skaitin� form� ir
     * nustatomas serverio adreso strukt�roje.
     */    
    if ( inet_aton(argv[1], &servaddr.sin_addr) <= 0 ) {
        fprintf(stderr,"ERROR #3: Invalid remote IP address.\n");
        exit(1);
    }

    
    /* 
     * Prisijungiama prie serverio
     */
    if (connect(s_socket,(struct sockaddr*)&servaddr,sizeof(servaddr))<0){
        fprintf(stderr,"ERROR #4: error in connect().\n");
        exit(1);
    }
    
    printf("Enter the message: ");
    fgets(buffer, BUFFLEN, stdin);
    /*
     * I�siun�iamas prane�imas serveriui
     */
    send(s_socket,buffer,strlen(buffer),0);

    memset(&buffer,0,BUFFLEN);
    /*
     * Prane�imas gaunamas i� serverio
     */
    recv(s_socket,buffer,BUFFLEN,0);
    printf("Server sent: %s\n", buffer);

    /*
     * Socket'as u�daromas
     */
    close(s_socket);
    return 0;
}
