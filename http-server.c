#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

static void die(const char *s) { perror(s); exit(1); }

int main(int argc, char **argv)
{
    // ignore SIGPIPE so that we don't terminate when we call send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    if (argc != 5) {
        fprintf(stderr, "usage: %s <server_port> <web_root> <mdb-lookup-host> <mdb-lookup-port>\n", argv[0]);
        exit(1);
    }

    unsigned short servPort = atoi(argv[1]);

    // create server socket
    int servsock;
    if ((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socked failed");

    // construct local address structure
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // any network interface
    servaddr.sin_port = htons(servPort);

    // bind to local address
    if (bind(servsock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        die("bind failed");

    // start listening for incoming connections
    if (listen(servsock, 5 /* queue size for connection requests */ ) < 0)
        die("listen failed");

    int clntsock;
    socklen_t clntlen;
    struct sockaddr_in clntaddr;

    int mdbSock;
    if ((mdbSock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    struct sockaddr_in mdbaddr;
    struct hostent *he;
    char *mdbName = argv[3];

    // get mdb IP
    if ((he = gethostbyname(mdbName)) == NULL) {
        die("gethostbyname failed");
    }
    char *mdbIP = inet_ntoa(*(struct in_addr *)he->h_addr);

    memset(&mdbaddr, 0, sizeof(mdbaddr));
    mdbaddr.sin_family = AF_INET;
    mdbaddr.sin_addr.s_addr = inet_addr(mdbIP);
    unsigned short mdbPort = atoi(argv[4]);
    mdbaddr.sin_port = htons(mdbPort);

    // connect to mdb
    if (connect(mdbSock, (struct sockaddr *) &mdbaddr, sizeof(mdbaddr)) < 0)
        die("connect failed");

    FILE *mdbInput = fdopen(mdbSock, "r");
    if (mdbInput == NULL) {
        die("cannot fdopen mdb-lookup-server socket connection");
    }

    // accept incoming connection
    while (1) {
        clntlen = sizeof(clntaddr); // initializse the in-out parameter

        // accept() returns a connected socket (client socket) which stores the client's address
        if ((clntsock = accept(servsock,
                        (struct sockaddr *) &clntaddr, &clntlen)) < 0) {
            continue;
        }

        FILE *input = fdopen(clntsock, "r");
        if (input == NULL) {
            close(clntsock);
            continue; 
        }

        // parse through GET request
        char requestLine[1000];
        fgets(requestLine, sizeof(requestLine), input);
        if (requestLine == NULL) {
            fclose(input);
            continue;
        }
        if (ferror(input)) {
            fclose(input);
            continue;
        }
        
        // get client IP
        char *clntIP = inet_ntoa(clntaddr.sin_addr);

        // skip headers
        int flag = 0;
        char line[1000];
        for(;;) {
            if (fgets(line, sizeof(line), input) == NULL) {
                fclose(input);
                flag = 1;
                break;
            }
            if ((strcmp("\r\n", line) == 0) || (strcmp("\n", line) == 0)) {
                break;
            }
        }
        if (flag) {
            fprintf(stdout, "%s \" \" 400 Bad Request\n", clntIP);
            continue;
        }

        char *statusCode = "200";
        char *reasonPhrase = "OK";
        char savedRL[1000];

        // parse through request line
        int null = 0;
        char *token_separators = "\t \r\n"; // tab, space, new line
        char *method = strtok(requestLine, token_separators);
        if (method == NULL) {
            method = "(null)";
            null = 1;
        }
        snprintf(savedRL, sizeof(method), "%s ", method);

        char *requestURI = strtok(NULL, token_separators);
        if (requestURI == NULL) {
            requestURI = "(null)";
            null = 1;
        }
        strcat(savedRL, requestURI);

        char *httpVersion = strtok(NULL, token_separators);
        if (httpVersion == NULL) {
            httpVersion = "(null)";
            null = 1;
        }
        strcat(savedRL, " ");
        strcat(savedRL, httpVersion);

        // NOTE: functionality only works for GET
        if (strstr(method, "GET") == NULL) {
            char buf[1000];
            snprintf(buf, sizeof(buf), "HTTP/1.0 501 Not Implemented\r\n\r\n<html><body><h1>501 Not Implemented</h1></body></html>\r\n");
            size_t len = strlen(buf);
            statusCode = "501";
            reasonPhrase = "Not Implemented";
            send(clntsock, buf, len, 0);
            fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue;
        }

        // NOTE: functionality only works for HTTP/1.0 and 1.1
        if ((strcmp(httpVersion, "HTTP/1.1")) != 0 && (strcmp(httpVersion, "HTTP/1.0")) != 0) {
            char buf[1000];
            snprintf(buf, sizeof(buf), "HTTP/1.0 501 Not Implemented\r\n\r\n<html><body><h1>501 Not Implemented</h1></body></html>\r\n");
            size_t len = strlen(buf);
            statusCode = "501";
            reasonPhrase = "Not Implemented";
            send(clntsock, buf, len, 0);
            fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue;
        }

        // check request URI
        int URILen = strlen(requestURI);
        char *URIEnd = &requestURI[URILen - 3];

        if ((requestURI[0] != '/') || (strstr(requestURI, "/../") != NULL) || (strcmp(URIEnd, "/..") == 0)) {
           char buf[1000];
           snprintf(buf, sizeof(buf), "HTTP/1.0 400 Bad Request\r\n\r\n<html><body><h1>400 Bad Request</h1></body></html>\r\n");
           size_t len = strlen(buf);
           statusCode = "400";
           reasonPhrase = "Bad Request";
           send(clntsock, buf, len, 0);
           fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
           fclose(input);
           continue;
        }

        if (null) {
            statusCode = "501";
            reasonPhrase = "Not Implemented";
            char buf[1000];
            snprintf(buf, sizeof(buf), "HTTP/1.0 501 Not Implemented\r\n\r\n<html><body><h1>501 Not Implemented</h1></body></html>\r\n");
            size_t len = strlen(buf);
            send(clntsock, buf, len, 0);
            fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue;
        }

        // if request URI ends with /, append index.html
        if (requestURI[strlen(requestURI) - 1] == '/') {
            strcat(requestURI, "index.html");
        }

        // hardcode /mdb-lookup form
        if (strcmp(requestURI, "/mdb-lookup") == 0) {
            const char *form = 
                "<h1>mdb-lookup</h1>\n"
                "<p>\n"
                "<form method=GET action=/mdb-lookup>\n"
                "lookup: <input type=text name=key>\n"
                "<input type=submit>\n"
                "</form>\n"
                "<p>\n";

            char buf[1000];
            snprintf(buf, sizeof(buf), "HTTP/1.0 200 OK\r\n\r\n<html><body>%s</body></html>\r\n", form);
            size_t len = strlen(buf);            
            
            int failSend = 0;
            if (send(clntsock, buf, len, 0) != len) {
                fclose(input);
                failSend = 1;
                break;
            }
            if (failSend)
                continue;

            fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue;
        }

        // lookup result table
        if (strstr(requestURI, "/mdb-lookup?key=") != NULL) {

            // get search string 
            char *key = strrchr(requestURI, '=');
            key++;
            char searchString[strlen(key) + 2];
            snprintf(searchString, sizeof(searchString), "%s\n", key);

            // send search string to mdb-lookup-server
            send(mdbSock, searchString, strlen(searchString), 0);

            // read back the result rows
            // send form
            const char *form =
                "<h1>mdb-lookup</h1>\n"
                "<p>\n"
                "<form method=GET action=/mdb-lookup>\n"
                "lookup: <input type=text name=key>\n"
                "<input type=submit>\n"
                "</form>\n"
                "<p>\n";

            char line[1000];
            char buffer[1000];
            snprintf(line, sizeof(line), "HTTP/1.0 200 OK\r\n\r\n<html><body>%s\r\n<p><table border>\r\n", form);
            send(clntsock, line, strlen(line), 0);
            fgets(buffer, sizeof(buffer), mdbInput);
            while ((strcmp(buffer, "\n")) != 0) {
                snprintf(line, sizeof(line), "<tr><td>%s\r\n", buffer); 
                send(clntsock, line, strlen(line), 0);
                fgets(buffer, sizeof(buffer), mdbInput);
            }
            snprintf(line, sizeof(line), "</table></body></html>\r\n");
            send(clntsock, line, strlen(line), 0);

            // log
            fprintf(stdout, "looking up [%s]: %s \"%s\" %s %s\n", key, clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue; 
        }

        char directory[1000];
        strcpy(directory, argv[2]);

        // create path
        char *path = strcat(directory, requestURI);

        // determine if requestURI is a file or directory
        struct stat sb;
        if (stat(path, &sb) == -1) {
            statusCode = "404";
            reasonPhrase = "Not Found";
            char buf[1000];
            snprintf(buf, sizeof(buf), "HTTP/1.0 404 Not Found\r\n\r\n<html><body><h1>404 Not Found</h1></body></html>\r\n");
            size_t len = strlen(buf);
            send(clntsock, buf, len, 0);
            fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue;
        }

        int badDir = 0;
        if (S_ISDIR(sb.st_mode)) { 
            if (requestURI[strlen(requestURI) - 1] != '/') {
                statusCode = "403";
                reasonPhrase = "Forbidden";
                char buf[1000];
                snprintf(buf, sizeof(buf), "HTTP/1.0 403 Forbidden\r\n\r\n<html><body><h1>403 Forbidden</h1></body></html>\r\n");
                size_t len = strlen(buf);

                send(clntsock, buf, len, 0);
                fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
                fclose(input);
                badDir = 1;
            }
        }
        if (badDir)
            continue;

        // open requested file 
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            statusCode = "404";
            reasonPhrase = "Not Found";
            char buf[1000];
            snprintf(buf, sizeof(buf), "HTTP/1.0 404 Not Found\r\n\r\n<html><body><h1>404 Not Found</h1></body></html>\r\n");
            size_t len = strlen(buf);
            send(clntsock, buf, len, 0);
            fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);
            fclose(input);
            continue;
        }

        // Send OK
        char buffer[1000];
        snprintf(buffer, sizeof(buffer), "HTTP/1.0 200 OK\r\n\r\n");
        size_t length = strlen(buffer);
        send(clntsock, buffer, length, 0);

        // Read file in chunks
        size_t n;
        char chunk[4096];
        while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) { 
            send(clntsock, chunk, n, 0);
        }

        if (ferror(fp)) {
            fclose(fp);
            continue;
        }
        
        // log to output
        fprintf(stdout, "%s \"%s\" %s %s\n", clntIP, savedRL, statusCode, reasonPhrase);   
        fclose(fp);

        // close socket after sending response
        fclose(input);
    }
    return 0;
}
