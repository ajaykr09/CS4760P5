#include <unistd.h>
#include <sys/types.h>
#include <iostream>
#include <cstdlib>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <ctime>

#define PERMS 0644
#define NUMBER_OF_RESOURCES 10
#define REQUEST_CODE 10
#define TERMINATION_CODE 21
#define MILLISECOND_TIMER 250000000
#define REQUEST 0
#define RELEASE 1
#define RESOURCE_INSTANCES 20

using namespace std;

struct msgbuffer {
    long mtype;
    int intData;
    pid_t childPid;
};

struct resourceTracker {
    int requests[NUMBER_OF_RESOURCES];
    int allocations[NUMBER_OF_RESOURCES];
};

int RNG(int max) {
    return (rand() % max);
}

int decideAction() {
    int choice = RNG(100);
    if(choice < 90)
        return REQUEST;
    return RELEASE;
}

int chooseRequestResource(struct resourceTracker *resourceTracker) {
    int chosenResource = RNG(9);
    int remainingRequests;
    for (int count = 0; count < NUMBER_OF_RESOURCES; count++) {
        remainingRequests = RESOURCE_INSTANCES - resourceTracker->allocations[chosenResource];
        if (resourceTracker->allocations[chosenResource] < 20 && resourceTracker->requests[chosenResource] < remainingRequests)
            return chosenResource;
        if (chosenResource > 0)
            chosenResource--;
        else
            chosenResource = 9;
    }
    return -1;
}


int chooseReleaseResource(struct resourceTracker *resourceTracker) {
    int chosenResource = RNG(9);
    for (int count = 0; count < NUMBER_OF_RESOURCES; count++) {
        if (resourceTracker->allocations[chosenResource] > 0)
            return chosenResource;
        if (chosenResource > 0)
            chosenResource--;
        else
            chosenResource = 9;
    }

    return -1;
}

int checkForTermination(struct resourceTracker *resourceTracker) {
    // If there are more requests for a resource than allocations, do not terminate
    for (int count = 0; count < NUMBER_OF_RESOURCES; count++) {
        if (resourceTracker->requests[count] > resourceTracker->allocations[count]) {
            return 0;
        }
    }

    return 1;
}

int addRequest(struct resourceTracker *resourceTracker, int resourceNumber) {
    if (resourceTracker->requests[resourceNumber] >= 20)
        return 0;
    resourceTracker->requests[resourceNumber] = resourceTracker->requests[resourceNumber] + 1;
    return 1;
}

void addAllocation(struct resourceTracker *resourceTracker, int allocationNumber) {
    int resourceNumber = allocationNumber;
    resourceTracker->allocations[resourceNumber] = resourceTracker->allocations[resourceNumber] + 1;
}

void removeRequest(struct resourceTracker *resourceTracker, int resourceNumber) {
    resourceTracker->requests[resourceNumber] = resourceTracker->requests[resourceNumber] - 1;
}

void removeAllocation(struct resourceTracker *resourceTracker, int allocationNumber) {
    int resourceNumber = allocationNumber - REQUEST_CODE;
    resourceTracker->allocations[resourceNumber] = resourceTracker->allocations[resourceNumber] - 1;
}

void initializeResourceTracker(struct resourceTracker *resourceTracker) {
    for(int count = 0; count < NUMBER_OF_RESOURCES; count++) {
        resourceTracker->allocations[count] = 0;
        resourceTracker->requests[count] = 0;
    }
}

int main(int argc, char** argv) {
    // Get access to shared memory
    const int sh_key = ftok("./oss.c", 0);
    int shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
    int *simulatedClock = (int*)shmat(shm_id, 0, 0);

    msgbuffer buf;
    buf.mtype = 1;
    buf.intData = 0;
    int msqid = 0;
    key_t key;

    // Get a key for our message queue
    if ((key = ftok("msgq.txt", 1)) == -1) {
        perror("ftok");
        exit(1);
    }

    // Create our message queue
    if ((msqid = msgget(key, PERMS)) == -1) {
        perror("msgget in child");
        exit(1);
    }

    pid_t parentPid = getppid();
    pid_t myPid = getpid();

    struct resourceTracker *resourceTracker;
    resourceTracker = (struct resourceTracker*)malloc(sizeof(struct resourceTracker));
    initializeResourceTracker(resourceTracker);

    unsigned int randval;
    FILE *f;
    f = fopen("/dev/urandom", "r");
    fread(&randval, sizeof(randval), 1, f);
    fclose(f);

    srand(randval);

    int timer = simulatedClock[1];
    int aliveTime = simulatedClock[0];
    int terminate = 0;

    msgbuffer rcvbuf;
    while (1) {
        aliveTime = simulatedClock[0] - aliveTime;
        if (aliveTime >= 1 && simulatedClock[1] - timer >= MILLISECOND_TIMER) {
            terminate = checkForTermination(resourceTracker);
            timer = simulatedClock[1];

            if (terminate) {
                buf.intData = TERMINATION_CODE;
                if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
                    printf("msgsnd to parent failed.\n");
                    exit(1);
                }
                shmdt(simulatedClock);
                return EXIT_SUCCESS;
            }
        }

        buf.mtype = parentPid;
        buf.childPid = myPid;

        int action = decideAction();

        if (REQUEST == action) {
            buf.intData = chooseRequestResource(resourceTracker);
            if (buf.intData == -1) {
                exit(0);
            }
        }

        if (RELEASE == action) {
            buf.intData = chooseReleaseResource(resourceTracker);
            if (buf.intData == -1) {
                buf.intData = chooseRequestResource(resourceTracker);
            }
            else {
                buf.intData += 10;
            }
        }

        if (msgsnd(msqid, &buf, sizeof(msgbuffer), 0) == -1) {
            printf("msgsnd to parent failed.\n");
            exit(1);
        }

        if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer), myPid, 0) == -1) {
            printf("msgrcv failure in child %d\n", myPid);
            exit(1);
        }

        if (1 == rcvbuf.intData) {
            removeRequest(resourceTracker, buf.intData);
            addAllocation(resourceTracker, buf.intData);
        }
        else if (2 == rcvbuf.intData) {
            removeAllocation(resourceTracker, buf.intData);
        }
        else {
            do {
                if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer), myPid, 0) == -1) {
                    printf("msgrcv failure in child %d\n", myPid);
                    exit(1);
                }
            } while (rcvbuf.intData != 1);
            removeRequest(resourceTracker, buf.intData);
            addAllocation(resourceTracker, buf.intData);
        }
    }

    shmdt(simulatedClock);
    return EXIT_SUCCESS;
}
