#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <ctime>
#include <csignal>
#include <sys/msg.h>
#include <cerrno>
#include <queue>
#include <vector>
#include <cstdio>
#include <iostream>

#define PERMS 0644
#define MAX_CHILDREN 18
#define ONE_SECOND 1000000000
#define HALF_SECOND 500000000
#define STANDARD_CLOCK_INCREMENT 100000
#define RESOURCE_TABLE_SIZE 10

typedef struct msgBuffer {
    long mtype;
    int intData;
    pid_t childPid;
} msgBuffer;

struct resource {
    int totalInstances;
    int availableInstances;
};

struct Queue {
    int front, rear, size;
    unsigned capacity;
    std::vector<int> array;
};

// For storing each child's PCB. Memory is allocated in main
struct PCB {
    bool occupied; // either true or false
    pid_t pid; // process id of this child
    int startTimeSeconds; // time when it was created
    int startTimeNano; // time when it was created
    int blocked; // describes which resource this process is waiting for
    std::vector<int> requestVector; // represents how many instances of each resource have been requested
    std::vector<int> allocationVector; // represents how many instances of each resource have been granted
};

std::vector<PCB> processTable;

// For storing resources
std::vector<resource> resourceTable;
// Message queue id
int msqid;
// Needed for killing all child processes
int processTableSize;
// Needed for launching purposes
int runningChildren;
// Output file
FILE *fptr;
// Shared memory variables
int sh_key;
int shm_id;
int *simulatedClock;
// Message buffer for passing messages
msgBuffer buf;
// Queue for keeping track of blocked (sleeping) processes
Queue *sleepQueue;

// Process table functions
void initializeProcessTable();
void startInitialProcesses(int initialChildren);
void initializePCB(pid_t pid);
void outputProcessTable();

// OSS functions
void incrementClock(int timePassed);
void launchChild(int maxSimulChildren, int launchInterval, int *lastLaunchTime);
void checkForMessages();
void updateTable(pid_t process, msgBuffer rcvbuf);
void nonblockWait();
void startInitialProcesses(int initialChildren);
void checkOutstandingRequests();

// Program end functions
void terminateProgram(int signum);
void sighandler(int signum);

// Log file functions
void sendingOutput(int chldNum, int chldPid, int systemClock[2]);
void receivingOutput(int chldNum, int chldPid, int systemClock[2], msgBuffer rcvbuf);

// Resource Functions
void grantResource(pid_t childPid, int resourceNumber, int processNumber);
void initializeResourceTable();
void request(pid_t childPid, int resourceNumber);
int release(pid_t childPid, int resourceNumber, int output);
void outputResourceTable();
int runDeadlockDetection();

struct Queue* createQueue(unsigned capacity);
int isFull(struct Queue* queue);
int isEmpty(struct Queue* queue);
void enqueue(struct Queue* queue, int item);
int dequeue(struct Queue* queue);
int front(struct Queue* queue);

// Helper functions
int checkChildren(int maxSimulChildren);
int stillChildrenToLaunch();
int childrenInSystem();
int findTableIndex(pid_t pid);
void checkTime(int* outputTimer, int* deadlockDetectionTimer);
void takeAction(pid_t childPid, int msgData);
void childTerminated(pid_t terminatedChild);
void sendMessage(pid_t childPid, int msg, int output);
void deadlockTermination();

int main(int argc, char** argv) {
    // Signals to terminate program properly if user hits ctrl+c or 60 seconds pass
    alarm(60);
    signal(SIGALRM, sighandler);
    signal(SIGINT, sighandler);

    // Allocate shared memory
    sh_key = ftok("./oss.c", 0);
    shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
    if (shm_id <= 0) {
        std::cout << "Shared memory allocation failed\n";
        exit(1);
    }

    // Attach to shared memory
    simulatedClock = (int*)shmat(shm_id, 0, 0);
    if (simulatedClock <= 0) {
        std::cout << "Attaching to shared memory failed\n";
        exit(1);
    }

    // Set clock to zero
    simulatedClock[0] = 0;
    simulatedClock[1] = 0;

    runningChildren = 0;

    // Message queue setup
    key_t key;
    system("touch msgq.txt");

    // Get a key for our message queue
    if ((key = ftok("msgq.txt", 1)) == -1) {
        perror("ftok");
        exit(1);
    }

    // Create our message queue
    if ((msqid = msgget(key, PERMS | IPC_CREAT)) == -1) {
        perror("msgget in parent");
        exit(1);
    }

    // User input variables
    int option;
    int proc;
    int simul;
    int timelimit;
     // Initialize the process table and resource table
    initializeProcessTable();
    initializeResourceTable();
    outputProcessTable();

    while ((option = getopt(argc, argv, "hn:s:i:f:")) != -1) {
        switch (option) {
            case 'h':
                printf(" [-n proc] [-s simul] [-t timelimitForChildren]\n"
 "[-i intervalInMsToLaunchChildren] [-f logfile]");
                return 0;
                break;
            case 'n':
                proc = atoi(optarg);
                break;
            case 's':
                simul = atoi(optarg);
                break;
            case 'i':
                timelimit = atoi(optarg);
                break;
            case 'f':
                fptr = fopen(optarg, "a");
                break;
        }
    }

    // Sets the global var equal to the user arg
    processTableSize = proc;

    // Allocates memory for the processTable stored in global memory
    processTable = (struct PCB*)calloc(processTableSize, sizeof(struct PCB));
    initializeProcessTable();

    resourceTable = (struct resource*)malloc(RESOURCE_TABLE_SIZE * sizeof(struct resource));

    initializeResourceTable();

    startInitialProcesses(simul);

    int* outputTimer = new int;
    *outputTimer = 0;

    int* deadlockDetectionTimer = new int;
    *deadlockDetectionTimer = 0;

    int* lastLaunchTime = new int;
    *lastLaunchTime = 0;

    sleepQueue = createQueue(processTableSize);

    return 0;
}

int main(int argc, char* argv[]) {

while (stillChildrenToLaunch() || childrenInSystem()) {

    // Nonblocking waitpid to see if a child has terminated.
    nonblockWait();
    launchChild(simul, timelimit, lastLaunchTime);

    // Try to grant any outstanding requests
    checkOutstandingRequests();
    checkForMessages();

    // Run deadlock algo every sec
    checkTime(outputTimer, deadlockDetectionTimer);

    incrementClock(STANDARD_CLOCK_INCREMENT);
}

pid_t wpid;
int status = 0;
while ((wpid = wait(&status)) > 0);
terminateProgram(SIGTERM);
return EXIT_SUCCESS;
}


void checkOutstandingRequests() {
    if (isEmpty(sleepQueue))
        return;

    int sleepQueueSize = sleepQueue->size;
    for (int processCounter = 0; processCounter < sleepQueueSize; processCounter++) {
        int currentPid = dequeue(sleepQueue);
        int entry = findTableIndex(currentPid);
        if (!processTable[entry].occupied)
            continue;

        for (int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
            if (!processTable[entry].requestVector[resourceCounter])
                continue;

            if (resourceTable[resourceCounter].availableInstances > 0) {
                fprintf(fptr, "MASTER: Waking process %d\n", currentPid);
                processTable[entry].blocked = 0;
                grantResource(currentPid, resourceCounter, entry);
                return;
            }
        }
        enqueue(sleepQueue, currentPid);
    }
}

void startInitialProcesses(int initialChildren) {
    int lowerValue = (initialChildren < processTableSize) ? initialChildren : processTableSize;

    for (int count = 0; count < lowerValue; count++) {
        pid_t newChild;
        newChild = fork();
        if (newChild < 0) {
            perror("Fork failed");
            exit(-1);
        } else if (newChild == 0) {
            char fakeArg[sizeof(int)];
            snprintf(fakeArg, sizeof(int), "%d", 1);
            execlp("./worker", fakeArg, NULL);
            exit(1);
        } else {
            initializePCB(newChild);
            printf("MASTER: Launching Child PID %d\n", newChild);
            runningChildren++;
        }
    }
}

void nonblockWait() {
    int status;
    pid_t terminatedChild = waitpid(0, &status, WNOHANG);

    if (terminatedChild <= 0)
        return;

    childTerminated(terminatedChild);
}

void childTerminated(pid_t terminatedChild) {
    int entry = findTableIndex(terminatedChild);
    for (int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
        while (release(terminatedChild, count, 0));
        processTable[entry].requestVector[count] = 0;
    }
    processTable[entry].occupied = 0;
    processTable[entry].blocked = 0;
    runningChildren--;

    fprintf(fptr, "MASTER: Child pid %d has terminated and its resources have been released.\n", terminatedChild);
}

void checkForMessages() {
    msgBuffer rcvbuf;
    if (msgrcv(msqid, &rcvbuf, sizeof(msgBuffer), getpid(), IPC_NOWAIT) == -1) {
        if (errno == ENOMSG) {
        } else {
            printf("Got an error from msgrcv\n");
            perror("msgrcv");
            terminateProgram(6);
        }
    } else if (rcvbuf.childPid != 0) {
        takeAction(rcvbuf.childPid, rcvbuf.intData);
    }
}

void takeAction(pid_t childPid, int msgData) {
    if (msgData < 10) {
        request(childPid, msgData);
        return;
    }
    if (msgData < 20) {
        release(childPid, (msgData - RESOURCE_TABLE_SIZE), 1);
        return;
    }
    childTerminated(childPid);
}

void request(pid_t childPid, int resourceNumber) {
    int entry = findTableIndex(childPid);
    processTable[entry].requestVector[resourceNumber] += 1;
    fprintf(fptr, "MASTER: Child pid %d has requested an instance of resource %d\n", childPid, resourceNumber);
    grantResource(childPid, resourceNumber, entry);
}

int release(pid_t childPid, int resourceNumber, int output) {
    int entry = findTableIndex(childPid);
    if (processTable[entry].allocationVector[resourceNumber] > 0) {
        processTable[entry].allocationVector[resourceNumber] -= 1;
        if (output)
            fprintf(fptr, "MASTER: Child pid %d has released an instance of resource %d\n", childPid, resourceNumber);
        resourceTable[resourceNumber].availableInstances += 1;
        sendMessage(childPid, 2, output);
        return 1;
    }
    if (output)
        fprintf(fptr, "MASTER: Child pid %d has attempted to release an instance of resource %d that it does not have\n", childPid, resourceNumber);
    return 0;
}

void grantResource(pid_t childPid, int resourceNumber, int processNumber) {
    if (!processTable[processNumber].occupied)
        return;

    buf.mtype = childPid;
    if (resourceTable[resourceNumber].availableInstances > 0) {
        processTable[processNumber].allocationVector[resourceNumber] += 1;
        processTable[processNumber].requestVector[resourceNumber] -= 1;

        resourceTable[resourceNumber].availableInstances -= 1;

        fprintf(fptr, "MASTER: Requested instance of resource %d to child pid %d has been granted.\n", resourceNumber, childPid);
        sendMessage(childPid, 1, 1);
    }
    else {
        fprintf(fptr, "MASTER: Requested instance of resource %d to child pid %d has been denied.\n", resourceNumber, childPid);
        sendMessage(childPid, 0, 1);
        int entry = findTableIndex(childPid);
        processTable[entry].blocked = resourceNumber + 1;
        enqueue(sleepQueue, childPid);
    }
}

void sendMessage(pid_t childPid, int msg, int output) {
    buf.intData = msg;
    buf.mtype = childPid;
    if (output)
        fprintf(fptr, "MASTER: Sending message of %d to child pid %d\n", msg, childPid);
    if (msgsnd(msqid, &buf, sizeof(msgBuffer), 0) == -1) {
        perror("msgsnd to child failed\n");
        terminateProgram(6);
    }
}

void checkTime(int* outputTimer, int* deadlockDetectionTimer) {
    if (std::abs(simulatedClock[1] - *outputTimer) >= HALF_SECOND) {
        *outputTimer = simulatedClock[1];
        *deadlockDetectionTimer += 1;
        printf("\nOSS PID:%d SysClockS:%d SysClockNano:%d\n", getpid(), simulatedClock[0], simulatedClock[1]);
        outputProcessTable();
        outputResourceTable();
    }
    if (2 == *deadlockDetectionTimer) {
        *deadlockDetectionTimer = 0;

        //Terminates processes using the most resources first
        while (runDeadlockDetection()) {
            deadlockTermination();
        }
    }
}

void deadlockTermination() {
    int heaviestProcess;
    int currentResourcesUsed = 0;
    int mostResourcesUsed = 0;

    for (int processCounter = 0; processCounter < processTableSize; processCounter++) {
        for (int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
            currentResourcesUsed += processTable[processCounter].allocationVector[resourceCounter];
        }
        if (currentResourcesUsed > mostResourcesUsed) {
            mostResourcesUsed = currentResourcesUsed;
            heaviestProcess = processCounter;
        }
        currentResourcesUsed = 0;
    }
    pid_t workerToTerminate = processTable[heaviestProcess].pid;
    fprintf(fptr, "MASTER: Killing child pid %d to try and correct deadlock.\n", workerToTerminate);
    int sleepQueueSize = sleepQueue->size;
    for (int count = 0; count < sleepQueueSize; count++) {
        int currentPid = dequeue(sleepQueue);
        if (currentPid == workerToTerminate) {
            break;
        }
        enqueue(sleepQueue, currentPid);
    }
    kill(workerToTerminate, SIGKILL);
    childTerminated(workerToTerminate);
}

int runDeadlockDetection() {
    fprintf(fptr, "MASTER: Running deadlock detection algorithm at time %d.%d\n", simulatedClock[0], simulatedClock[1]);
    int requestMatrix[processTableSize][RESOURCE_TABLE_SIZE];
    int allocationMatrix[processTableSize][RESOURCE_TABLE_SIZE];
    int availableVector[RESOURCE_TABLE_SIZE];
    for (int processCounter = 0; processCounter < processTableSize; processCounter++) {
        for (int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
            requestMatrix[processCounter][resourceCounter] = processTable[processCounter].requestVector[resourceCounter];
            allocationMatrix[processCounter][resourceCounter] = processTable[processCounter].allocationVector[resourceCounter];
            availableVector[resourceCounter] = resourceTable[resourceCounter].availableInstances;
        }
    }

    int mostResourceIntensiveProcess = -1;
    int mostResourcesUsed = 0;

    for (int processCounter = 0; processCounter < processTableSize; processCounter++) {
        int currentResourcesUsed = 0;
        for (int resourceCounter = 0; resourceCounter < RESOURCE_TABLE_SIZE; resourceCounter++) {
            currentResourcesUsed += allocationMatrix[processCounter][resourceCounter];
        }
        if (currentResourcesUsed > mostResourcesUsed) {
            mostResourcesUsed = currentResourcesUsed;
            mostResourceIntensiveProcess = processCounter;
        }
    }

    if (mostResourceIntensiveProcess != -1) {
        fprintf(fptr, "MASTER: Deadlock detected. Process %d is the most resource-intensive.\n", processTable[mostResourceIntensiveProcess].pid);
        return processTable[mostResourceIntensiveProcess].pid;
    } else {
        fprintf(fptr, "MASTER: No deadlock detected. Continuing.\n");
        return 0;
    }
}

// Sets all initial pid values to 0
void initializeProcessTable() {
    for (int count = 0; count < processTableSize; count++) {
        processTable[count].pid = 0;
    }
}

// Initializes values of the PCB
void initializePCB(pid_t pid) {
    int index = 0;

    while (processTable[index].pid != 0)
        index++;

    processTable[index].occupied = 1;
    processTable[index].pid = pid;
    processTable[index].startTimeSeconds = simulatedClock[0];
    processTable[index].startTimeNano = simulatedClock[1];
    processTable[index].blocked = 0;
}

void initializeResourceTable() {
    for (int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
        resourceTable[count].availableInstances = 20;
        resourceTable[count].totalInstances = 20;
    }
}

void launchChild(int maxSimulChildren, int launchInterval, int* lastLaunchTime) {
    if ((simulatedClock[1] - *lastLaunchTime) < launchInterval)
        return;

    if (checkChildren(maxSimulChildren) && stillChildrenToLaunch()) {
        pid_t newChild;
        newChild = fork();
        if (newChild < 0) {
            perror("Fork failed");
            exit(-1);
        }
        else if (newChild == 0) {
            char fakeArg[sizeof(int)];
            snprintf(fakeArg, sizeof(int), "%d", 1);
            execlp("./worker", fakeArg, NULL);
            exit(1);
        }
        else {
            initializePCB(newChild);
            *lastLaunchTime = simulatedClock[1];
            fprintf(fptr, "MASTER: Launching new child pid %d.\n", newChild);
            runningChildren++;
        }
    }
}

bool checkChildren(int maxSimulChildren) {
    return runningChildren < maxSimulChildren;
}

bool stillChildrenToLaunch() {
    return processTable[processTableSize - 1].pid == 0;
}

// Returns true if any children are running. Returns false otherwise
bool childrenInSystem() {
    for (int count = 0; count < processTableSize; count++) {
        if (processTable[count].occupied) {
            return true;
        }
    }
    return false;
}

// Returns the buffer index corresponding to a given pid
int findTableIndex(pid_t pid) {
    for (int count = 0; count < processTableSize; count++) {
        if (processTable[count].pid == pid)
            return count;
    }
    return -1;
}

void incrementClock(int timePassed) {
    simulatedClock[1] += timePassed;
    if (simulatedClock[1] >= ONE_SECOND) {
        simulatedClock[1] -= ONE_SECOND;
        simulatedClock[0] += 1;
    }
}

void terminateProgram(int signum) {
    // Kills any remaining active child processes
    for (int count = 0; count < processTableSize; count++) {
        if (processTable[count].occupied)
            kill(processTable[count].pid, signum);
    }

    // Frees allocated memory
    delete[] processTable;
    processTable = nullptr;

    // Get rid of message queue
    if (msgctl(msqid, IPC_RMID, NULL) == -1) {
        perror("msgctl to get rid of queue in parent failed");
        exit(1);
    }

    // Close the log file
    fclose(fptr);

    // Detach from and delete memory
    shmdt(simulatedClock);
    shmctl(shm_id, IPC_RMID, NULL);

    printf("Program is terminating. Goodbye!\n");
    exit(1);
}

void sighandler(int signum) {
    printf("\nCaught signal %d\n", signum);
    terminateProgram(signum);
}

void outputProcessTable() {
    printf("%s\n%-8s %-8s %8s %8s %8s %8s\n", "Process Table:", "Entry", "Occupied", "PID", "StartS", "StartN", "Blocked");
    fprintf(fptr, "%s\n%-8s %-8s %8s %8s %8s %8s\n", "Process Table:", "Entry", "Occupied", "PID", "StartS", "StartN", "Blocked");
    for (int i = 0; i < processTableSize; i++) {
        printf("%-8d %-8d %8d %8d %8d %8d\n\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startTimeSeconds, processTable[i].startTimeNano, processTable[i].blocked);
        fprintf(fptr, "%-8d %-8d %8d %8d %8d %8d\n\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startTimeSeconds, processTable[i].startTimeNano, processTable[i].blocked);
    }
}

void outputResourceTable() {
    printf("%s\n%-8s %-8s %-8s\n", "Resource Table:", "Entry", "Available", "Total");
    fprintf(fptr, "%s\n%-8s %-8s %-8s\n", "Resource Table:", "Entry", "Available", "Total");
    for (int count = 0; count < RESOURCE_TABLE_SIZE; count++) {
        printf("%-8d %-8d %-8d\n", count, resourceTable[count].availableInstances, resourceTable[count].totalInstances);
        fprintf(fptr, "%-8d %-8d %-8d\n", count, resourceTable[count].availableInstances, resourceTable[count].totalInstances);
    }
}

void sendingOutput(int chldNum, int chldPid, int systemClock[2]) {
    fprintf(fptr, "OSS:\t Sending message to worker %d PID %d at time %d:%d\n", chldNum, chldPid, systemClock[0], systemClock[1]);
}

void receivingOutput(int chldNum, int chldPid, int systemClock[2], msgBuffer rcvbuf) {
    if (rcvbuf.intData != 0) {
        fprintf(fptr, "OSS:\t Receiving message from worker %d PID %d at time %d:%d\n", chldNum, chldPid, systemClock[0], systemClock[1]);
    } else {
        printf("OSS:\t Worker %d PID %d is planning to terminate.\n", chldNum, chldPid);
        fprintf(fptr, "OSS:\t Worker %d PID %d is planning to terminate.\n", chldNum, chldPid);
    }
}
}

struct Queue {
    int front, rear, size;
    unsigned capacity;
    int* array;
};

// Function to create a queue
Queue* createQueue(unsigned capacity) {
    Queue* queue = new Queue();
    queue->array = std::vector<int>(capacity);
    queue->capacity = capacity;
    queue->front = queue->size = 0;
    queue->rear = capacity - 1;
    return queue;
}

int isFull(Queue* queue) {
    return (queue->size == queue->capacity);
}

// Queue is empty when size is 0
int isEmpty(Queue* queue) {
    return (queue->size == 0);
}

void enqueue(Queue* queue, int item)
{
    if (isFull(queue))
        return;
    queue->rear = (queue->rear + 1)
                  % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
}

int dequeue(Queue* queue)
{
    if (isEmpty(queue))
        return 0;
    int item = queue->array[queue->front];
    queue->front = (queue->front + 1)
                   % queue->capacity;
    queue->size = queue->size - 1;
    return item;
}

// Function to get front of queue
int front(Queue* queue)
{
    if (isEmpty(queue))
        return 0;
    return queue->array[queue->front];
}

// Function to get rear of queue
int rear(Queue* queue)
{
    if (isEmpty(queue))
        return 0;
    return queue->array[queue->rear];
}
