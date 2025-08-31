#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdbool.h>
#include <limits.h>

#define MAX_ELEVATORS 100
#define MAX_FLOORS 500
#define MAX_REQUEST_PASSENGER 30
#define MAX_ELEV_CAPACITY 20


typedef struct PassengerRequest 
{
      int requestId;
      int startFloor;
      int requestedFloor;
}PassengerRequest;

typedef struct mainSharedMemory
{
      char authStrings[100][MAX_ELEV_CAPACITY + 1];
      char elevatorMovementInstructions[100];
      PassengerRequest newPassengerRequests[MAX_REQUEST_PASSENGER];
      int elevatorFloors[100];
      int droppedPassengers[1000];
      int pickedUpPassengers[1000][2];
}mainSharedMemory;

typedef struct solverRequest
{
      long mtype;
      int elevatorNumber;
      char authStringGuess[MAX_ELEV_CAPACITY + 1];
}solverRequest;

typedef struct solverResponse
{
      long mtype;
      int guessIsCorrect;
}solverResponse;

typedef struct turnChangeResponse
{
      long mtype;
      int turnNumber;
      int newPassengerRequestCount;
      int errorOccured;
      int finished;
}turnChangeResponse;

typedef struct turnChangeRequest
{
      long mtype;
      int droppedPassengersCount;
      int pickedPassengersCount;
}turnChangeRequest;

typedef struct elevStatus
{
      int currentFloor;
      int direction;
      int cntPassengers;
}elevStatus;

typedef struct globRequests
{
      int requestId;
      int pickFloor;
      int dropFloor;
      int status;
      int closetElevator;
}globRequests;


int N, K, M, T;
key_t sharedMemoryKey, mainMsgQueueKey;
elevStatus elevators[MAX_ELEVATORS+1];

mainSharedMemory *mainShmPtr;

int mainMsgQueueId;
int solverMsgQueueId[MAX_ELEVATORS+1];


globRequests requests[10000];
int position;

int msgSolverIndex;

int dropIndex, pickIndex;

void generateAllStrings(char guessString[], int currIndex, int stringLength, int selectedElevator, int *foundString)
{
      if(*foundString == 1)
            return;
      
      if(currIndex == stringLength)
      {
            solverRequest msgRequest;
            solverResponse msgResponse;

            for(int i = 0; i < stringLength; i++)
            {
                  msgRequest.authStringGuess[i] = guessString[i];
            }
            msgRequest.authStringGuess[stringLength] = '\0';
            msgRequest.mtype = 3;

            if(msgsnd(solverMsgQueueId[msgSolverIndex%M], &msgRequest, sizeof(msgRequest) - sizeof(long), 0) == -1)
            {
                  perror("ERROR IN SENDING MESSAGE\n");
            } 
            if(msgrcv(solverMsgQueueId[msgSolverIndex%M], &msgResponse, sizeof(msgResponse) - sizeof(long),4, 0) == -1)
            {
                  perror("ERROR IN RECEIVING MESSAGE\n");
            }

            if(msgResponse.guessIsCorrect == 1)
            {
                  *foundString = 1;
                  
                  for(int i = 0; i < stringLength; i++)
                  {
                        mainShmPtr->authStrings[selectedElevator][i] = msgRequest.authStringGuess[i];
                  }
                  mainShmPtr->authStrings[selectedElevator][stringLength] = '\0';
            }
            return;
      }

      for(char ch = 'a'; ch <= 'f'; ch++)
      {
            guessString[currIndex] = ch;
            generateAllStrings(guessString, currIndex + 1, stringLength, selectedElevator, foundString);
      }
}

void findAuthString(int selectedElevator, int stringLength)
{
      solverRequest msgRequest;
      msgRequest.mtype = 2;
      msgRequest.elevatorNumber = selectedElevator;

      if(msgsnd(solverMsgQueueId[msgSolverIndex%M], &msgRequest, sizeof(solverRequest) - sizeof(long), 0) == -1)
      {
            perror("ERROR IN SENDING MESSAGE");
      }

      char guessString[MAX_ELEV_CAPACITY + 1];
      for(int i=0; i < stringLength; i++)
      {
            guessString[i] = 'a';
      }
      guessString[stringLength] = '\0';

      int foundString = 0;

      generateAllStrings(guessString,0, stringLength,selectedElevator,&foundString);

      msgSolverIndex = (msgSolverIndex + 1)%M;

}

void handleDrops(int bestElevator, int currGap, int requestId)
{
      elevStatus *elevator = &elevators[bestElevator];
      globRequests *currReq = &requests[requestId];
      if(currGap == 0)
      {
            mainShmPtr->elevatorMovementInstructions[bestElevator] = 's';
            mainShmPtr->droppedPassengers[dropIndex] = currReq->requestId;
            elevator->direction = 0;
            dropIndex++;
            currReq->status = 0;
            currReq->closetElevator = -1;
            elevator->cntPassengers--;
      }
      else if(currGap > 0)
      {
            mainShmPtr->elevatorMovementInstructions[bestElevator] = 'd';
            elevator->direction = -1;
      }
      else 
      {
            mainShmPtr->elevatorMovementInstructions[bestElevator] = 'u';
            elevator->direction = 1;
      }
}


void handlePickups(int bestElevator, int currGap, int requestId)
{
      elevStatus *elevator = &elevators[bestElevator];
      globRequests *currReq = &requests[requestId];
      if(currGap == 0)
      {
            mainShmPtr->elevatorMovementInstructions[bestElevator] = 'u';
            mainShmPtr->pickedUpPassengers[pickIndex][0] = currReq->requestId ;
            mainShmPtr->pickedUpPassengers[pickIndex][1] = bestElevator;
            elevator->direction = 1;
            pickIndex++;
            currReq->status = -1;
            currReq->closetElevator = bestElevator;
            elevator->cntPassengers++;
      }
      else if(currGap > 0)
      {
            mainShmPtr->elevatorMovementInstructions[bestElevator] = 'd';
            elevator->direction = -1;
      }
      else 
      {
            mainShmPtr->elevatorMovementInstructions[bestElevator] = 'u';
            elevator->direction = 1;
      }
}

void findPickupElevator(int *minDist, int *minGap, int *bestElevator,int requestId)
{
      globRequests *currReq = &requests[requestId];
      for(int currElev = 0; currElev < N; currElev++)
      {
            if((elevators[currElev].cntPassengers >= 4))
                  continue;

            int currGap = mainShmPtr->elevatorFloors[currElev] - currReq->pickFloor;

            if(elevators[currElev].direction * currGap <= 0)
            {
                  if(abs(currGap) < *minDist)
                  {
                        *bestElevator = currElev;
                        *minDist = abs(currGap);
                        *minGap = currGap;
                  }
            }
      }
}

void findDropElevator(int *minDist, int *minGap, int *bestElevator,int requestId)
{     
      *bestElevator = requests[requestId].closetElevator;
      *minGap = mainShmPtr->elevatorFloors[*bestElevator] - requests[requestId].dropFloor;
      *minDist = abs(*minGap);
}

void setDirectionsForElevator() 
{
      for(int i=0; i < N; i++)
      {
            if(elevators[i].direction == 0) 
            {
                  mainShmPtr->elevatorMovementInstructions[i] = 's';
            }
            else if(elevators[i].direction == 1)
            {
                  if(mainShmPtr->elevatorFloors[i] == K - 1)
                        mainShmPtr->elevatorMovementInstructions[i] = 's';
                  else
                        mainShmPtr->elevatorMovementInstructions[i] = 'u';
            }
            else 
            {
                  if(mainShmPtr->elevatorFloors[i] == 0)
                        mainShmPtr->elevatorMovementInstructions[i] = 's';
                  else
                        mainShmPtr->elevatorMovementInstructions[i] = 'd';
            }
      }
}

int requestHandling()
{
      dropIndex = 0;
      pickIndex = 0;

      turnChangeResponse turnResponse;
      if(msgrcv(mainMsgQueueId,  &turnResponse, sizeof(turnChangeResponse) - sizeof(long), 2, 0) == -1)
      {
            exit(1);
      }
      if(turnResponse.finished == 1)
      {
            return 1;
      }
      if(turnResponse.errorOccured == 1)
      {
            exit(1);
      }
      
      for(int i=0; i < turnResponse.newPassengerRequestCount; i++)
      {
            PassengerRequest currReq = mainShmPtr->newPassengerRequests[i];
            requests[position].requestId =  currReq.requestId;
            requests[position].pickFloor = currReq.startFloor;
            requests[position].dropFloor = currReq.requestedFloor;
            requests[position].status = 1;
            requests[position].closetElevator = -1;
            position++;
      }

      for(int i=0; i < N; i++)
      {
            elevators[i].currentFloor = mainShmPtr->elevatorFloors[i];
      }

      for(int i=0; i < N; i++)
      {     
            elevStatus *elevator = &elevators[i];
            if(elevator->cntPassengers > 0)
            {
                  findAuthString(i, elevator->cntPassengers);
            }
      }

      for(int i=0; i < position; i++)
      {
            globRequests *currReq = &requests[i];
            
            int minDist = 10000;
            int minGap = 10000;    
            int bestElevator = 10000;
            
            if(currReq->status == 0)
                  continue;

            if(currReq->status > 0)
            {
                  findPickupElevator(&minDist,&minGap,&bestElevator,i);
            }
            else
            {
                  findDropElevator(&minDist,&minGap,&bestElevator,i);
            }
            if(bestElevator == 10000)
            {
                  continue;
            }
            elevStatus *elevator = &elevators[bestElevator];

            if(currReq->status < 0)
            {
                  handleDrops(bestElevator, minGap, i);
            }
            else
            {
                  handlePickups(bestElevator, minGap, i);
            }
      }
      
      setDirectionsForElevator();
      
      turnChangeRequest turnRequest;
      turnRequest.mtype = 1;
      turnRequest.droppedPassengersCount = dropIndex;
      turnRequest.pickedPassengersCount = pickIndex;
      if(msgsnd(mainMsgQueueId,&turnRequest,sizeof(turnChangeRequest) - sizeof(long),0 ) == -1)
      {
            exit(1);
      }
      return 0;
}





int main(int argc, char *argv[])
{
      
      char inputFile[20] = "input.txt";
      FILE *input = fopen(inputFile, "r");

      fscanf(input,"%d %d %d %d",&N,&K,&M,&T);

      fscanf(input, "%d %d",&sharedMemoryKey,&mainMsgQueueKey);

      key_t solverMsgQueueKey[M+1];
      for(int i=0; i < M; i++)
      {
            fscanf(input,"%d",&solverMsgQueueKey[i]);
      }
      
      fclose(input);

      position = 0;
      msgSolverIndex = 0;

      int mainShmId = shmget(sharedMemoryKey, sizeof(mainSharedMemory), 0);
      if(mainShmId == -1)
      {
            return 1;
      }
      mainShmPtr = shmat(mainShmId, NULL, 0);
      if (mainShmPtr == (void *)-1) 
      { 
            return 1;
      }
      
      mainMsgQueueId = msgget(mainMsgQueueKey, 0666);

      for(int i=0; i < M; i++)
      {
            solverMsgQueueId[i] = msgget(solverMsgQueueKey[i],0666);
      }

      for (int i = 0; i < N; i++) 
      {
            elevators[i].currentFloor = 0;
            elevators[i].direction = 0; 
            elevators[i].cntPassengers = 0;
      }

      int flag = 0;

      while(!flag)
      {
            flag = requestHandling();
      }
      
}
