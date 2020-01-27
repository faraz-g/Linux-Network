#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>

/**
 * Struct which holds the information describing a resource,
 * the resource name and the amount of that resource at the depot. 
 */ 
typedef struct {
    char* resource;
    int amount;
} Resource;

/**
 * Contains the information for a neighbouring depot. Provides means
 * of communication by holding FILE* for communication to and from.  
 */
typedef struct {
    char* name;
    int portNo;
    FILE* to;
    FILE* from;
} Neighbour;

/**
 * Represents the depot. Holds this depot's network info, neighbours, 
 * and resources. 
 */
typedef struct {
    int numColons;
    int numResources;
    int serverSocket;
    int portNo;
    int numNeighbours;
    char* name;
    Resource* resources;
    Neighbour* neighbours;
} Depot;

/**
 * Holds the information for a defer command. The key, the arguments 
 * to execute, and whether the command has been executed or not.
 */
typedef struct {
    long key;
    char* args;
    bool complete;
} Defer;

/**
 * Holds the information passed to the thread handlers when threading 
 * for new clients. Contains all of the information of the network.
 */
typedef struct {
    Depot* depot;
    int deferCount;
    int clientSocket;
    int clientNum;
    int msgCount;
    int portNo;
    FILE* to;
    FILE* from;
    Defer* deferred;
    bool imSent;
    bool imRecieved;
} ThreadInfo;

void ignore_sigpipe();
void* sigcatcher(void* v);
char* is_name_valid(char* name);
int is_amount_valid(char* amount);
void gather_resources(Depot* depot, int numResources, char** resources);
int lexo_cmp(const void* a, const void* b);
int neigh_cmp(const void* a, const void* b);
void sort_resources(Depot* depot);
void sort_neigh(Depot* depot);
void init_server(Depot* depot);
void create_threads(Depot* depot);
void* client_connections(void* input);
void validate_input(char* input, ThreadInfo* threadInfo);
void do_input(char** args, int msg, ThreadInfo* threadInfo);
int verify_num(char input[256]);
char* verify_name(char input[256]);
void* new_connection(void* input);
void connect_message(char** args, ThreadInfo* threadInfo);
void im_message(char** args, ThreadInfo* threadInfo);
void deliver_message(char** args, ThreadInfo* threadInfo);
void withdraw_message(char** args, ThreadInfo* threadInfo);
void transfer_message(char** args, ThreadInfo* threadInfo);
void defer_message(char** args, ThreadInfo* threadInfo);
void execute_message(char** args, ThreadInfo* threadInfo);
char* im_creator(Depot* depot);
char* defer_creator(char** args);
char* i_to_s_converter(int input);

int main(int argc, char** argv) {
    Depot* depot = malloc(sizeof(depot) * 16);
    pthread_t tid;
    sigset_t set;
    if (argc < 2) {
        fprintf(stderr, "Usage: 2310depot name {goods qty}\n");
        exit(1);
    }
    if (!is_name_valid(argv[1])) {
        fprintf(stderr, "Invalid name(s)\n");
        exit(2);
    }
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, 0);
    pthread_create(&tid, 0, sigcatcher, (void*) depot);
    depot->name = argv[1]; 
    gather_resources(depot, argc - 2, argv);
    ignore_sigpipe();
    init_server(depot);
}

/**
 * Ignores sigpipe such that the program does not terminate
 * upon a lost connection.
 * 
 * Params: void
 * Return: void
 */
void ignore_sigpipe() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPIPE, &sa, 0);
}

/**
 * Thread handler for handling sighup. When sighup is recieved,
 * prints the depot's neighbours and goods in a lexographically
 * sorted manner.
 * 
 * Params: (void* input) input points to the depot struct. 
 * Return: NULL
 */
void* sigcatcher(void* input) {
    Depot* depot = (Depot*) input;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    int num;
    while (!sigwait(&set, &num)) { 
        printf("Goods:\n");
        fflush(stdout);
        sort_resources(depot);
        sort_neigh(depot);
        for (int i = 0; i < depot->numResources; i++) {
            if (depot->resources[i].amount != 0) {
                printf("%s %d\n", depot->resources[i].resource, 
                        depot->resources[i].amount);
            }
            fflush(stdout);
        }
        printf("Neighbours:\n");
        fflush(stdout);
        for (int i = 0; i < depot->numNeighbours; i++) {
            printf("%s\n", depot->neighbours[i].name);
            fflush(stdout);
        }
    }
    return 0;
}

/**
 * For argv() processing. Returns the input string if it is valid, exitting
 * and setting the error code appropriately otherwise.
 * 
 * Params: (char* name) name is the string to process.
 * Return: (char*) the verified string
 */
char* is_name_valid(char* name) {
    if (strlen(name) == 0) {
        fprintf(stderr, "Usage: 2310depot name {goods qty}\n");
        exit(1);
    }
    char* invalid = " \n\r:";
    for (int i = 0; i < (strlen(name)); i++) {
        for (int j = 0; j < 4; j++) {
            if (name[i] == invalid[j]) {
                fprintf(stderr, "Invalid name(s)\n");
                exit(2);
            }
        }
    }
    return name;
}

/**
 * For argv() processing. Returns the input string
 * as an int if it is valid, exitting and setting 
 * the error code appropriately otherwise.
 * 
 * Params: (char* amount) amount is the qty to process. 
 * Return: (int) the verified quantity.
 */
int is_amount_valid(char* amount) {
    char* ptr;
    long output = strtol(amount, &ptr, 10);
    if (!(atoi(amount) && (output >= 0) && (strlen(ptr) == 0))) {
        fprintf(stderr, "Invalid quantity\n");
        exit(3);       
    }
    return ((int) output);
}

/**
 * Takes as input argc and argv from main. Loops through every entry
 * in the input string array ensuring that they are valid goods and 
 * quanities. The quantities and amounts are then ammended to an array
 * of Resources in the Depot struct.
 * 
 * Params: (Depot* depot, int numResources, char** resources) 
 * Return: Void
 */
void gather_resources(Depot* depot, int numResources, char** resources) {
    Resource* loot = malloc(sizeof(Resource) * 2048);
    int j = 0;
    for (int i = 0; i < numResources; i++) {
        if (!(i % 2)) {
            loot[j].resource = is_name_valid(resources[i + 2]);
        } else {
            loot[j++].amount = is_amount_valid(resources[i + 2]);
        }
    }
    depot->numResources = j;
    depot->resources = loot;
}

/**
 * Defines a comparator for comparing two Resource structs.
 * The names of the resources are compared using strcmp, and the 
 * value is returned.
 * 
 * Params: (const void* a, const void* b) 
 * Return: (int) as per strcmp.
 */
int lexo_cmp(const void* a, const void* b) {
    const Resource* resource1 = (Resource*) a;
    const Resource* resource2 = (Resource*) b; 
    return strcmp(resource1->resource, resource2->resource);
}

/**
 * Defines a comparator for comparing two Neighbour structs.
 * The names of the neighbours are compared using strcmp, and the 
 * value is returned.
 * 
 * Params: (const void* a, const void* b) 
 * Return: (int) as per strcmp.
 */
int neigh_cmp(const void* a, const void* b) {
    const Neighbour* neigh1 = (Neighbour*) a;
    const Neighbour* neigh2 = (Neighbour*) b;
    return strcmp(neigh1->name, neigh2->name);
}

/**
 * Sorts the neighbours currently added to the Depot using the 
 * neigh_cmp comparator.
 * 
 * Params: (Depot* depot) pointer to the depot struct.
 * Return: Void
 */
void sort_neigh(Depot* depot) {
    qsort(depot->neighbours, depot->numNeighbours, 
            sizeof(Neighbour), neigh_cmp);
}

/**
 * Sorts the resources currently added to the Depot using the 
 * lexo_cmp comparator.
 * 
 * Params: (Depot* depot) pointer to the depot struct.
 * Return: Void
 */
void sort_resources(Depot* depot) {
    qsort(depot->resources, depot->numResources, sizeof(Resource), lexo_cmp);
}

/**
 * Initilises the server for this depot. Creating threads after
 * the initilisation which then accept every new connectin on a 
 * unique thread.
 * 
 * Params: (Depot* depot) pointer to the depot struct.
 * Return: void
 */
void init_server(Depot* depot) {
    int serverSocket;
    int numNeighbours;
    int portNo;
    struct sockaddr_in addressInfo;
    Neighbour* neighbours = malloc(sizeof(Neighbour) * 256);
    memset(&addressInfo, 0, sizeof(addressInfo));
    addressInfo.sin_family = AF_INET;
    addressInfo.sin_addr.s_addr = INADDR_ANY;
    addressInfo.sin_port = 0;

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    bind(serverSocket, (struct sockaddr*) &addressInfo, sizeof(addressInfo));
    listen(serverSocket, 5);

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    socklen_t len = sizeof(address);
    getsockname(serverSocket, (struct sockaddr*) &address, &len);
    portNo = (int) ntohs(address.sin_port);
    numNeighbours = 0;
    depot->portNo = portNo;
    depot->numNeighbours = numNeighbours;
    depot->neighbours = neighbours;
    depot->serverSocket = serverSocket;
    printf("%d\n", portNo);
    fflush(stdout);
    create_threads(depot);
}

/**
 * Creates threads for each new connection to the server.
 * 
 * Params: (Depot* depot) pointer to the depot struct.
 * Return: void
 */
void create_threads(Depot* depot) {
    int serverSocket = depot->serverSocket, clientSocket, numClients = 0;
    struct sockaddr_in client;
    socklen_t address = sizeof(client);
    client.sin_family = AF_INET;
    client.sin_addr.s_addr = INADDR_ANY;
    client.sin_port = 0;

    ThreadInfo* threadInfo = malloc(sizeof(ThreadInfo) * 2);

    while(clientSocket = accept(serverSocket, 
            (struct sockaddr*) &client, &address), clientSocket >= 0) {
        pthread_t tid;
        numClients++;
        threadInfo->depot = depot;
        threadInfo->clientNum = numClients - 1;
        threadInfo->clientSocket = clientSocket;
        pthread_create(&tid, 0, client_connections, (void*) threadInfo);
    }
}

/**
 * Thread handler for creating new connections for each client of
 * the server. Creates FILE* variables which are stored in the 
 * structs for means of communicating to and from the client.
 * 
 * Params: (void* input) The input contains a pointer to a ThreadInfo
 * struct.
 * Return: NULL;
 */
void* client_connections(void* input) {
    ThreadInfo* threadInfo = (ThreadInfo*) input;
    Defer* defers = malloc(sizeof(Defer) * 1024);
    Depot* depot = threadInfo->depot;
    int clientSocket = threadInfo->clientSocket;
    int fromSocket = dup(clientSocket);
    threadInfo->msgCount = 0;
    FILE* to = fdopen(clientSocket, "w");
    FILE* from = fdopen(fromSocket, "r");

    threadInfo->to = to;
    threadInfo->from = from;

    char* inputMessage = malloc(sizeof(char) * 256);
    char* outputMessage = im_creator(depot);
    
    fprintf(to, "%s", outputMessage);
    fflush(to);

    threadInfo->imSent = true;
    threadInfo->imRecieved = false;
    threadInfo->deferred = defers;
    threadInfo->deferCount = 0;

    while(fgets(inputMessage, 256, from)) {
        if (threadInfo->msgCount > 1) {
            if (!(threadInfo->imRecieved && threadInfo->imSent)) {
                break;
            }
        } 
        validate_input(inputMessage, threadInfo);
        threadInfo->msgCount++;
    }
    return NULL;
}

/**
 * Processes the input stream recieved from the client, treating ':' as a 
 * delimiter and assigning every argument into a 2D char array to be 
 * passed onto subsequent functions.
 * 
 * Params: (char* input, ThreadInfo* threadInfo) newline input recieved
 * from client and a pointer to the ThreadInfo struct containing the 
 * information relevant to this thread.
 * Return: void
 */
void validate_input(char* input, ThreadInfo* threadInfo) {
    Depot* depot = threadInfo->depot;
    char** args = malloc(sizeof(char*) * 256);
    for (int i = 0; i < 256; i++) {
        args[i] = malloc(sizeof(char) * 256);
    }
    //Valid commands.
    char messages[10][10] = {"Connect", "IM", "Deliver", 
            "Withdraw", "Transfer", "Defer", "Execute"};
    int i = 0, j = 0, c = 0, numColons = 0;
    //Fills in args, indexing along by using ':' as a delimiter
    while (input[c] != '\n') {
        if (input[c] == ':') {
            args[i][j] = '\0';
            j = 0;
            c++;
            i++;
            numColons++;
        }
        if (input[c] == '\n') {
            break;
        }
        args[i][j++] = input[c++];
    }
    depot->numColons = numColons;
    for (int i = 0; i < 7; i++) {
        if (strcmp(args[0], messages[i]) == 0) {
            do_input(args, i, threadInfo);
            free(args);
        }
    }
}

/**
 * Calls the relevant function depending on which of the 7 possible
 * commands are being called by the client.
 * 
 * Params: (char** args, int msg, ThreadInfo* threadInfo)
 * Return: void
 */
void do_input(char** args, int msg, ThreadInfo* threadInfo) {
    switch (msg) {
        case 0:
            connect_message(args, threadInfo);
            break;
        case 1:
            im_message(args, threadInfo);
            break;
        case 2:
            deliver_message(args, threadInfo);
            break;
        case 3:
            withdraw_message(args, threadInfo);
            break;
        case 4:
            transfer_message(args, threadInfo);
            break;
        case 5:
            defer_message(args, threadInfo);
            break;
        case 6:
            execute_message(args, threadInfo);
            break;
        default:
            break;
    }
}

/**
 * Helper method similar to is_amount_valid as before. Does not
 * exit() upon failure, instsead returning 0.
 * 
 * Params: (char input[256]) character array containing to be 
 * converted to int.
 * Return: (int) the converted int.
 */
int verify_num(char input[256]) {
    char* ptr;
    long output = strtol(input, &ptr, 10);
    int portNum;
    if (strlen(ptr) == 0 && output > 0) {
        portNum = (int) output;
        return portNum;
    }
    return 0;
}

/**
 * Helper method similar to verify_name as before. Does not
 * exit() upon failure, instsead returning an empty string.
 * 
 * Params: (char input[256]) character array containing to be 
 * converted to int.
 * Return: (char*) the input string if verified, "" otherwise.
 */
char* verify_name(char input[256]) {
    char* name = input;
    char* invalid = " \n\r:";
    char* invalidReturn = "";
    if (!strlen(name) == 0) {
        for (int i = 0; i < (strlen(name)); i++) {
            for (int j = 0; j < 4; j++) {
                if (name[i] == invalid[j]) {
                    return invalidReturn;
                }
            }
        }
        return name;
    }
    return invalidReturn;
}

/**
 * Thread handler for the connect message. Listens to the port specified 
 * from connect and deals with the input recieved.
 * 
 * Params: (void* input) Pointer to the ThreadInfo struct containing
 * connection information.
 * Return: NULL
 */
void* new_connection(void* input) {
    ThreadInfo* threadInfo = (ThreadInfo*) input;
    Defer* defers = malloc(sizeof(Defer) * 1024);
    struct sockaddr_in addressInfo;
    memset(&addressInfo, 0, sizeof(addressInfo));
    addressInfo.sin_family = AF_INET;
    addressInfo.sin_addr.s_addr = INADDR_ANY;
    addressInfo.sin_port = htons(threadInfo->portNo);
    int newSock = socket(AF_INET, SOCK_STREAM, 0);
    int fromSocket = dup(newSock);
    connect(newSock, (struct sockaddr*) &addressInfo, sizeof(addressInfo));

    FILE* to = fdopen(newSock, "w");
    FILE* from = fdopen(fromSocket, "r");

    char* inputMessage = malloc(sizeof(char) * 256);
    char* outputMessage = im_creator(threadInfo->depot);

    fprintf(to, "%s", outputMessage);
    fflush(to);
    
    threadInfo->msgCount = 0;
    threadInfo->imSent = true;
    threadInfo->imRecieved = false;
    threadInfo->to = to;
    threadInfo->from = from;     
    threadInfo->deferCount = 0;
    threadInfo->deferred = defers;

    while(fgets(inputMessage, 256, from)) {
        if (threadInfo->msgCount > 1) {
            if (!(threadInfo->imRecieved && threadInfo->imSent)) {
                break;
            }
        } 
        validate_input(inputMessage, threadInfo);
        threadInfo->msgCount++;
    }
    return NULL;
}

/**
 * Deals with the connect message, creating new threads for each connect
 * request recieved.
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void connect_message(char** args, ThreadInfo* threadInfo) {
    ThreadInfo* newConnection = malloc(sizeof(ThreadInfo) * 2);
    memcpy(&newConnection, &threadInfo, sizeof(threadInfo));
    int portNum = verify_num(args[1]);
    newConnection->portNo = portNum;
    if (portNum != 0 && threadInfo->imRecieved == true) {
        fflush(stdout);
        pthread_t tid;
        pthread_create(&tid, 0, new_connection, (void*) newConnection);
    }
}

/**
 * Deals with the IM requests recieved. Adds the information of the 
 * client who sent the IM to the neighbours list of the depot if 
 * not already a neighbour. Ignores IM requests from invalid ports and
 * client names. 
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void im_message(char** args, ThreadInfo* threadInfo) {
    fflush(stdout);
    Depot* depot = threadInfo->depot;
    int portNum = verify_num(args[1]);
    char* depotName = verify_name(args[2]);
    Neighbour neighbour;
    bool neighbourFound = false;
    int numColons = depot->numColons;
    if (!(portNum == 0 || strcmp(depotName, "") == 0) && 
            numColons == 2 && !(threadInfo->imRecieved)) {
        neighbour.name = depotName;
        neighbour.portNo = portNum;
        for (int i = 0; i < depot->numNeighbours; i++) {
            if (strcmp(neighbour.name, depot->neighbours[i].name) == 0) {
                neighbourFound = true;
            } else if (neighbour.portNo == depot->neighbours[i].portNo) {
                neighbourFound = true;
            }
        }
        if (!neighbourFound) {
            depot->neighbours[depot->numNeighbours++] = neighbour;
            depot->neighbours[depot->numNeighbours - 1].to = threadInfo->to;
            depot->neighbours[depot->numNeighbours - 1].from
                    = threadInfo->from;
            threadInfo->imRecieved = true;
            fflush(stdout);
        }
    }
}

/**
 * Delivers the specified amount of the specified good to the depot's 
 * resources. Processes the input as usual. 
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void deliver_message(char** args, ThreadInfo* threadInfo) {
    Depot* depot = threadInfo->depot;
    int amount = verify_num(args[1]);
    char* good = verify_name(args[2]);
    bool delivered = false;
    int numColons = depot->numColons;
    if (amount > 0 && numColons == 2) {
        for (int i = 0; i < depot->numResources; i++) {
            if (strcmp(good, depot->resources[i].resource) == 0
                    && !delivered) {
                depot->resources[i].amount = 
                        depot->resources[i].amount + amount;
                delivered = true;
            }
        }
        if (!delivered) {
            depot->resources[depot->numResources].resource = good;
            depot->resources[depot->numResources].amount = amount;
            depot->numResources++;
        }
    }
}

/**
 * Withdraws the specified amount of the specified good from the depot's 
 * resources. Processes the input as usual. 
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void withdraw_message(char** args, ThreadInfo* threadInfo) {
    Depot* depot = threadInfo->depot;
    int amount = verify_num(args[1]);
    char* good = verify_name(args[2]);
    bool delivered = false;
    int numColons = depot->numColons;
    if (amount > 0 && numColons == 2) {
        for (int i = 0; i < depot->numResources; i++) {
            if (strcmp(good, depot->resources[i].resource)
                    == 0 && !delivered) {
                depot->resources[i].amount = 
                        depot->resources[i].amount - amount;
                delivered = true;
            }
        }
        if (!delivered) {
            depot->resources[depot->numResources].resource = good;
            depot->resources[depot->numResources].amount = 0 - amount;
            depot->numResources++;
        }
    }
}

/**
 * Withdraws the specified amount of the specified good from the depot's
 * resources if the destination's name is in the depot's neighbours. 
 * Sends a deliver message to the destination depot. 
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void transfer_message(char** args, ThreadInfo* threadInfo) {
    Depot* depot = threadInfo->depot;
    char** messageArgs = malloc(sizeof(char*) * 256);
    for (int i = 0; i < 256; i++) {
        messageArgs[i] = malloc(sizeof(char) * 256);
    }
    messageArgs[1] = args[1];
    messageArgs[2] = args[2];
    depot->numColons = 2;
    bool found = false;
    FILE* to;
    for (int i = 0; i < depot->numNeighbours; i++) {
        if (!found) {
            if (strcmp(args[3], depot->neighbours[i].name) == 0) {
                found = true;
                withdraw_message(messageArgs, threadInfo);
                to = depot->neighbours[i].to;
                fflush(to);
                fprintf(to, "Deliver:%s:%s\n", args[1], args[2]);
                fflush(to);
            }
        }
    }
}

/**
 * Processes a defer command, adding the defer request to a list of Defers
 * in the depot.
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void defer_message(char** args, ThreadInfo* threadInfo) {
    char* defArgs = malloc(sizeof(char) * 1024);
    char* ptr;
    defArgs = defer_creator(args);
    long key = strtol(args[1], &ptr, 10);
    if (strlen(ptr) == 0 && key > 0 && strlen(defArgs) > 0) {
        threadInfo->deferred[threadInfo->deferCount++].args = defArgs;
        threadInfo->deferred[threadInfo->deferCount - 1].key = key;
        threadInfo->deferred[threadInfo->deferCount - 1].complete = false;
    }
}

/**
 * Processes an execute command. Looks for the input key in the list of 
 * defers held by the depot, executing every command which 
 * has the same key. Marks the defer request as completed upon execution.
 * 
 * Params: (char** args, ThreadInfo* threadInfo) args contains the 
 * arguments processed in validate_input.
 * Return: void.
 */
void execute_message(char** args, ThreadInfo* threadInfo) {
    int numToExec = 0;
    char* ptr;
    long key = strtol(args[1], &ptr, 10);
    if (strlen(ptr) == 0 && key > 0) {
        char** toExec = malloc(sizeof(char*) * threadInfo->deferCount);
        for (int i = 0; i < threadInfo->deferCount; i++) {
            if (key == threadInfo->deferred[i].key && 
                    threadInfo->deferred[i].complete == false) {
                toExec[numToExec++] = threadInfo->deferred[i].args;
                threadInfo->deferred[i].complete = true;
            }
        }
        for (int i = 0; i < numToExec; i++) {
            validate_input(toExec[i], threadInfo);
        }
    }
}

/**
 * Formats the IM message sent by the server upon a successful connection.
 * 
 * Params: (Depot* depot) the depot.
 * Return: (char*) the IM string.
 */
char* im_creator(Depot* depot) {
    char* output = malloc(sizeof(char) * 256);
    int port = depot->portNo;
    char* portNo = i_to_s_converter(port);
    char* depotName = depot->name;
    strcat(output, "IM:");
    strcat(output, portNo);
    strcat(output, ":");
    strcat(output, depotName);
    strcat(output, "\n");
    return output;   
}

/**
 * Creates the formattied command string for the list of defer
 * requests. Takes as input the 2D char array created originally 
 * by the validate_input function and transforms it back into 
 * a single stream of characters adding back in ':' as required.
 * 
 * Params: (char** args) args to process
 * Return: (char*) formatted stream.
 */
char* defer_creator(char** args) {
    char* output = malloc(sizeof(char) * 256);
    int numArgs = 0;
    for (int i = 2; i < 256; i++) {
        if (strlen(args[i]) != 0) {
            numArgs++;
        }
    }
    if (numArgs == 3) {
        strcat(output, args[2]);
        strcat(output, ":");
        strcat(output, args[3]);
        strcat(output, ":");
        strcat(output, args[4]);
        strcat(output, "\n");
    } else if (numArgs == 4) {
        strcat(output, args[2]);
        strcat(output, ":");
        strcat(output, args[3]);
        strcat(output, ":");
        strcat(output, args[4]);
        strcat(output, ":");
        strcat(output, args[5]);
        strcat(output, "\n");
    } else {
        output = "";
    }
    return output;
}

/**
 * Helpter method which converts an input integer to a string.
 * 
 * Params: (int input) the int to convert.
 * Return: (char*) converted string.
 */
char* i_to_s_converter(int input) {
    char nums[10] = "0123456789";
    char* p = malloc(sizeof(char) * 10);
    int i = input, digits = 0;
    if (input == 0) {
        p[0] = '0';
        p[1] = '\0';
        return p;
    }
    while(i) {
        ++p;
        i = i / 10;
        digits++;
        if (digits > 8) {
            p = realloc(p, sizeof(p) * 2);
            digits = 0;
        }
    }
    *p = '\0';
    while(input) {
        *--p = nums[input % 10];
        input = input / 10;
    }
    return p;
}