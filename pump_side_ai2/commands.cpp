#include "commands.h"

// Storage for the command queue + water ingress declared extern in commands.h.
Command cmdQueue[CMD_QUEUE_SIZE];
volatile int cmdHead = 0;
volatile int cmdTail = 0;
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;

volatile float        latestWater = 0.0f;
volatile bool         latestWaterValid = false;
volatile unsigned long lastWaterMs = 0;

// Tower session max/min — DISPLAY ONLY (see commands.h). Not part of failsafe.
volatile float        towerAvgMax = 0.0f;
volatile bool         towerAvgMaxValid = false;
volatile float        towerAvgMin = 0.0f;
volatile bool         towerAvgMinValid = false;
