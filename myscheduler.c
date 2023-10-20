#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <ctype.h>                      // for isalnum()
#include <math.h>                       // for ceil()



//  THESE CONSTANTS DEFINE THE MAXIMUM SIZE OF sysconfig AND command DETAILS
//  THAT YOUR PROGRAM NEEDS TO SUPPORT.  YOU'LL REQUIRE THESE CONSTANTS
//  WHEN DEFINING THE MAXIMUM SIZES OF ANY REQUIRED DATA STRUCTURES.

#define MAX_DEVICES                     4
#define MAX_DEVICE_NAME                 20
#define MAX_COMMANDS                    10
#define MAX_COMMAND_NAME                20
#define MAX_SYSCALLS_PER_PROCESS        40
#define MAX_RUNNING_PROCESSES           50

//  NOTE THAT DEVICE DATA-TRANSFER-RATES ARE MEASURED IN BYTES/SECOND,
//  THAT ALL TIMES ARE MEASURED IN MICROSECONDS (usecs),
//  AND THAT THE TOTAL-PROCESS-COMPLETION-TIME WILL NOT EXCEED 2000 SECONDS
//  (SO YOU CAN SAFELY USE 'STANDARD' 32-BIT ints TO STORE TIMES).

#define DEFAULT_TIME_QUANTUM            100

#define TIME_CONTEXT_SWITCH             5
#define TIME_CORE_STATE_TRANSITIONS     10
#define TIME_ACQUIRE_BUS                20

//  ----------------------------------------------------------------------

#define MAX_WORD                        20
#define UNKNOWN                         (-1)

#define STATE_READY                     0
#define STATE_RUNNING                   1
#define STATE_SLEEPING                  2
#define STATE_WAITING                   3
#define STATE_IO_BLOCKED                4
#define STATE_TERMINATED                5

//  DECLARE (NOT DEFINE) FUNCTIONS THAT ARE CALLED BEFORE BEING DEFINED
void DEBUG(char *fmt, ...);
void flush_DEBUG(int proc_on_CPU);

void append_to_READY_queue(int proc, char came_from[]);
int  find_device_byname(char name[]);

int USECS_SINCE_REBOOT      = 0;        // the global clock
int timequantum             = DEFAULT_TIME_QUANTUM;

bool verbose                = true;     // if debug printing required

void advance_time(int inc)
{
    if(inc > 1 && verbose) {
        DEBUG("transition takes %iusecs (%i..%i)", inc, USECS_SINCE_REBOOT+1, USECS_SINCE_REBOOT+inc);
        flush_DEBUG(UNKNOWN);

        for(int t=0 ; t < inc ; ++t) {
            USECS_SINCE_REBOOT += 1;
            DEBUG("+");
            flush_DEBUG(UNKNOWN);
        }
    }
    else {
        USECS_SINCE_REBOOT += inc;
    }
}

//  ----------------------------------------------------------------------

#define SYS_SPAWN                       0
#define SYS_READ                        1
#define SYS_WRITE                       2
#define SYS_SLEEP                       3
#define SYS_WAIT                        4
#define SYS_EXIT                        5

char *syscalls[] = {
    "spawn", "read", "write", "sleep", "wait", "exit", NULL
};

int find_syscall_byname(char name[])
{
    for(int s=0 ; syscalls[s] != NULL ; ++s) {
        if(strcmp(syscalls[s], name) == 0) {
            return s;
        }
    }
    printf("ERROR - syscall '%s' not found\n", name);
    exit(EXIT_FAILURE);
}

//  ----------------------------------------------------------------------

//  AN ARRAY OF STRUCTURES AND FUNCTIONS TO MANAGE THE SYSTEM'S KNOWN COMMANDS

struct {
    char        name[MAX_COMMAND_NAME+1];
    struct {
        int     when;                           // usecs of onCPU time
        int     which;                          // which system-call
        int     arg0;
        int     arg1;
        char    cmdname[MAX_COMMAND_NAME+1];    // iff spawn
    } syscalls[MAX_SYSCALLS_PER_PROCESS];
    int         nsyscalls;
} commands[MAX_COMMANDS];

int ncommands               = 0;
#define FOREACH_COMMAND     for(int c=0 ; c<ncommands ; ++c)

void add_command(char line[])
{
    if(sscanf(line, "%s", commands[ncommands].name) == 1) {
        commands[ncommands].nsyscalls       = 0;
        ++ncommands;
    }
}

int find_command_byname(char name[])
{
    FOREACH_COMMAND {
        if(strcmp(commands[c].name, name) == 0) {
            return c;
        }
    }
    printf("ERROR - command '%s' not found\n", name);
    exit(EXIT_FAILURE);
}

void add_syscall_to_command(char line[])
{
    int c = ncommands-1;
    int s = commands[c].nsyscalls;

    char usecs[MAX_WORD], word1[MAX_WORD], word2[MAX_WORD], word3[MAX_WORD];
    sscanf(line, "%s %s %s %s", usecs, word1, word2, word3);

    commands[c].syscalls[s].when    = atoi(usecs);
    commands[c].syscalls[s].which   = find_syscall_byname(word1);

    switch (commands[c].syscalls[s].which) {
        case SYS_SPAWN:
            strcpy(commands[c].syscalls[s].cmdname, word2);
            break;

        case SYS_READ:
        case SYS_WRITE:
            commands[c].syscalls[s].arg0    = find_device_byname(word2);
            commands[c].syscalls[s].arg1    = atoi(word3);
            break;

        case SYS_SLEEP:
            commands[c].syscalls[s].arg0    = atoi(word2);
            break;

        case SYS_WAIT:
        case SYS_EXIT:
            break;
    }
    ++commands[c].nsyscalls;
}

//  FIND COMMAND-NAMES FOR SYS_SPAWN, ENSURE A CALL TO SYS_EXIT
void patch_commands(void)
{
    FOREACH_COMMAND {
        bool exit_found = false;
        for(int s=0 ; s<commands[c].nsyscalls ; ++s) {
            if(commands[c].syscalls[s].which == SYS_SPAWN) {
                commands[c].syscalls[s].arg0    = find_command_byname(commands[c].syscalls[s].cmdname);
            }
            else if(commands[c].syscalls[s].which == SYS_EXIT) {
                exit_found  = true;
            }
        }
        if(!exit_found) {
            DEBUG("WARNING - command '%s' never calls 'exit'", commands[c].name);
            flush_DEBUG(UNKNOWN);
        }
    }
}

//  ----------------------------------------------------------------------

//  AN ARRAY OF STRUCTURES AND FUNCTIONS TO MANAGE THE SYSTEM'S PROCESSES

struct {
    int         state;                  // STATE_READY, STATE_RUNNING ...
    int         pid, ppid;
    int         time_on_CPU;
    int         command;                // index into commands[]
    int         next_syscall;           // index into commands[c].syscalls[]
    int         nchildren;              // other processes invoked by 'spawn'
} processes[MAX_RUNNING_PROCESSES];

int nprocesses              = 0;
#define FOREACH_PROCESS         for(int p=0 ; p<MAX_RUNNING_PROCESSES ; ++p)
#define PROCESS_SLOT_UNUSED(p)  (processes[p].pid == UNKNOWN)

void init_processes(void)
{
    FOREACH_PROCESS {
        processes[p].pid    = UNKNOWN;  // => unused slot
    }
    nprocesses      = 0;
}

//  SPAWN THE REQUESTED COMMAND, ADD TO THE READY QUEUE, ADD PARENT TO READY
void spawn_process(int command, int ppid)
{
    FOREACH_PROCESS {
        if(PROCESS_SLOT_UNUSED(p)) {
            static int next_pid             = 0;

            processes[p].state              = STATE_READY;
            processes[p].pid                = next_pid++;
            processes[p].ppid               = ppid;
            processes[p].command            = command;
            processes[p].next_syscall       = 0;
            processes[p].time_on_CPU        = 0;

            processes[p].nchildren          = 0;
            ++nprocesses;

            DEBUG("spawn '%s'", commands[command].name);
            append_to_READY_queue(p, "NEW");
            DEBUG("transition takes 0usecs");
            flush_DEBUG(UNKNOWN);
            return;
        }
    }
    printf("ERROR - process limit of %i exceeded\n", MAX_RUNNING_PROCESSES);
    exit(EXIT_FAILURE);
}

void exit_process(int proc_on_CPU)
{
    DEBUG("exit, pid%i.RUNNING->EXIT", processes[proc_on_CPU].pid);
    DEBUG("transition takes 0usecs");
    flush_DEBUG(processes[proc_on_CPU].command);

    FOREACH_PROCESS {
        if(PROCESS_SLOT_UNUSED(p)) {
            continue;
        }
        if(processes[p].pid == processes[proc_on_CPU].ppid) {   // found parent
            --processes[p].nchildren;
            break;                                              // as only one
        }
    }
    processes[proc_on_CPU].pid     = UNKNOWN;                   // now unused
    processes[proc_on_CPU].state   = STATE_TERMINATED;
    --nprocesses;
}

//  ----------------------------------------------------------------------

//  AN ARRAY OF STRUCTURES AND FUNCTIONS TO MANAGE THE SYSTEM'S I/O DEVICES

struct {
    char        name[MAX_DEVICE_NAME+1];
    int         read_speed;                 // Bps
    int         write_speed;                // Bps

    struct {
        int     proc;                       // index into processes[]
        int     syscall;                    // SYS_READ or SYS_WRITE
        int     nbytes;                     // size of request
    } blocked[MAX_RUNNING_PROCESSES];
    int         nblocked;

} devices[MAX_DEVICES];

int ndevices                = 0;
#define FOREACH_DEVICE      for(int d=0 ; d<ndevices ; ++d)

//  WHICH DEVICE CURRENTLY OWNS THE DATA-BUS AND IS BLOCKED?
int device_owning_databus   = UNKNOWN;
int databus_inuse_until     = UNKNOWN;
int nblocked                = 0;            // #blocked in all I/O queues

void init_devices_and_IO_BLOCKED_queues(void)
{
    FOREACH_DEVICE {
        devices[d].nblocked = 0;
    }

    device_owning_databus   = UNKNOWN;
    databus_inuse_until     = UNKNOWN;
    nblocked                = 0;
}

void add_device(char name[], int read_speed, int write_speed)
{
    strcpy(devices[ndevices].name, name);
    devices[ndevices].read_speed    = read_speed;
    devices[ndevices].write_speed   = write_speed;
    ++ndevices;
}

void append_to_IO_BLOCKED_queue(int proc_on_CPU, int syscall, int device, int nbytes)
{
    DEBUG("%s %ibytes, pid%i.RUNNING->BLOCKED", syscalls[syscall], nbytes, processes[proc_on_CPU].pid);
    advance_time(TIME_CORE_STATE_TRANSITIONS);

    int nb  = devices[device].nblocked;

    devices[device].blocked[nb].proc     = proc_on_CPU;
    devices[device].blocked[nb].syscall  = syscall;
    devices[device].blocked[nb].nbytes   = nbytes;
    ++devices[device].nblocked;
    ++nblocked;
}

//  AS ONLY ONE PROCESS CAN OWN THE DATABUS, ONLY ONE PROCESS WILL BE UNBLOCKED
void unblock_completed_IO(void)
{
    if(device_owning_databus != UNKNOWN && databus_inuse_until <= USECS_SINCE_REBOOT) {
        int proc    = devices[device_owning_databus].blocked[0].proc;
        int s       = devices[device_owning_databus].blocked[0].syscall;

        DEBUG("device.%s completes %s", devices[device_owning_databus].name, syscalls[s]);
        DEBUG("DATABUS is now idle");
        flush_DEBUG(UNKNOWN);

        append_to_READY_queue(proc, "BLOCKED");
        advance_time(TIME_CORE_STATE_TRANSITIONS);

        int nb  = devices[device_owning_databus].nblocked;
        for(int b=0 ; b < (nb-1) ; ++b) {               // 'slide' all left by 1
            devices[device_owning_databus].blocked[b]   = devices[device_owning_databus].blocked[b+1];
        }
        --devices[device_owning_databus].nblocked;
        --nblocked;

        device_owning_databus   = UNKNOWN;
        databus_inuse_until     = UNKNOWN;
    }
}

//  FIND THE FASTEST READING DEVICE READY TO PERFORM I/O
int find_fastest_ready_device(void)
{
    int fastest_ready_device    = UNKNOWN;
    int fastest_speed           = -1;

    FOREACH_DEVICE {
        if(devices[d].nblocked > 0 && devices[d].read_speed > fastest_speed) {
            fastest_ready_device    = d;
            fastest_speed           = devices[d].read_speed;
        }
    }
    return  fastest_ready_device;
}

void start_pending_IO(void)
{
//  IF NO DEVICE CURRENTLY OWNS (IS USING) THE DATABUS, AND OTHERS WISH TO
    if(device_owning_databus == UNKNOWN && nblocked > 0) {
        device_owning_databus   = find_fastest_ready_device();

//  DETERMINE HOW LONG THIS I/O WILL TAKE
        int s       = devices[device_owning_databus].blocked[0].syscall;
        int nbytes  = devices[device_owning_databus].blocked[0].nbytes;
        int speed;
        char *doing;

        if(s == SYS_READ) {
            speed   = devices[device_owning_databus].read_speed;
            doing   = "reading";
        }
        else /* SYS_WRITE */ {
            speed   = devices[device_owning_databus].write_speed;
            doing   = "writing";
        }

        int usecs               = ceil(1000000.0*(double)nbytes / (double)speed);
        databus_inuse_until     = USECS_SINCE_REBOOT + TIME_ACQUIRE_BUS + usecs;

        DEBUG("device.%s acquiring DATABUS, %s %i bytes, will take %iusecs (%i+%i)",
                devices[device_owning_databus].name, doing, nbytes,
                TIME_ACQUIRE_BUS+usecs, TIME_ACQUIRE_BUS, usecs);
        flush_DEBUG(UNKNOWN);
    }
}

int find_device_byname(char name[])
{
    FOREACH_DEVICE {
        if(strcmp(devices[d].name, name) == 0) {
            return d;
        }
    }
    printf("ERROR - device '%s' not found\n", name);
    exit(EXIT_FAILURE);
}

//  ----------------------------------------------------------------------

//  AN ARRAY AND FUNCTIONS TO MANAGE THE SYSTEM'S READY QUEUE

int READY_queue[MAX_RUNNING_PROCESSES];         // indicies into processes[]
int nready          = 0;

void init_READY_queue(void)
{
    nready          = 0;
}

void append_to_READY_queue(int proc, char came_from[])
{
    DEBUG("pid%i.%s->READY", processes[proc].pid, came_from);
    processes[proc].state   = STATE_READY;
    READY_queue[nready]     = proc;
    ++nready;
}

int dequeue_READY_queue(void)
{
    int proc    = UNKNOWN;

    if(nready > 0) {
        proc   = READY_queue[0];                // head of queue
        DEBUG("pid%i.READY->RUNNING", processes[proc].pid);
        advance_time(TIME_CONTEXT_SWITCH);

        for(int r=0 ; r<(nready-1) ; ++r) {     // 'slide' all left by 1
            READY_queue[r]  = READY_queue[r+1];
        }
        --nready;
    }
    return proc;
}

//  ----------------------------------------------------------------------

//  NOTHING TO UNDERSTAND IN THIS SECTION! - (see  man stdarg  if interested)
#include <stdarg.h>

#define MAX_DEBUG_LINES     100000

char    debugging[256]  = { '\0' };
char    *dp             = debugging;

void DEBUG(char *fmt, ...)
{
    if(verbose) {
        va_list ap;

        if(debugging[0]) {
            *dp++   = ',';
            *dp++   = ' ';
        }
        va_start(ap, fmt);
        vsprintf(dp, fmt, ap);
        va_end(ap);

        while(*dp) {
            ++dp;
        }
    }
}

void flush_DEBUG(int proc_on_CPU)
{
    if(debugging[0]) {                     // anything to output?
        char rhs[MAX_COMMAND_NAME+24];
        rhs[0]  = '\0';

        if(debugging[0] == '+') {
            strcpy(rhs, "OS");
        }
        else if(proc_on_CPU != UNKNOWN) {
            sprintf(rhs, "%s(onCPU=%i)",
                            commands[processes[proc_on_CPU].command ].name,
                            processes[proc_on_CPU].time_on_CPU);
        }
        printf("@%08i   %-80s%24s\n", USECS_SINCE_REBOOT, debugging, rhs);

        static  int     nlines          = 0;
        if(++nlines >= MAX_DEBUG_LINES) {
            printf("ERROR - too much debug output - giving up!\n");
            exit(EXIT_FAILURE);
        }
        debugging[0]    = '\0';
        dp              = debugging;
    }
}
#undef  MAX_DEBUG_LINES

//  ----------------------------------------------------------------------

#define CHAR_COMMENT            '#'

void read_sysconfig(char argv0[], char filename[])
{
    FILE    *fp = fopen(filename, "r");
    if(fp == NULL) {
        printf("%s: cannot open '%s'\n", argv0, filename);
        exit(EXIT_FAILURE);
    }

//  READ EACH LINE OF THE sysconfig FILE
    int lc=0;
    char line[BUFSIZ];
    while(fgets(line, sizeof line, fp) != NULL) {
        ++lc;
        if(line[0] == CHAR_COMMENT) {       // ignore this line
            continue;
        }

        char word0[MAX_WORD], word1[MAX_WORD], word2[MAX_WORD];

//  FOUND A DEVICE DEFINITION
        if(sscanf(line, "device %s %s %s", word0, word1, word2) == 3) {
            add_device(word0, atoi(word1), atoi(word2));
        }

//  FOUND THE timequantum
        else if(sscanf(line, "timequantum %s", word0) == 1) {
            timequantum = atoi(word0);
        }
        else {
            printf("ERROR - line %i of '%s' is not recognized\n", lc, filename);
            exit(EXIT_FAILURE);
        }
    }
    fclose(fp);
}

//  NOT REQUIRED, BUT PROVIDES A CHECK THAT THINGS HAVE BEEN STORED CORRECTLY
void dump_sysconfig(void)
{
    FOREACH_DEVICE {
        printf("%s\t%i\t%i\n", devices[d].name, devices[d].read_speed, devices[d].write_speed);
    }
    printf("#\ntimequantum\t%i\n#\n", timequantum);
}

//  ----------------------------------------------------------------------

void read_commands(char argv0[], char filename[])
{
    FILE    *fp = fopen(filename, "r");
    if(fp == NULL) {
        printf("%s: cannot open '%s'\n", argv0, filename);
        exit(EXIT_FAILURE);
    }

//  READ EACH LINE OF THE commands FILE
    int     lc=0;
    char    line[BUFSIZ];
    while(fgets(line, sizeof line, fp) != NULL) {
        ++lc;
        if(line[0] == CHAR_COMMENT) {       // ignore this line
            continue;
        }

//  FOUND A NEW COMMAND
        if(isalnum(line[0])) {
            add_command(line);
        }
//  FOUND A NEW SYSCALL FOR THE CURRENT COMMAND
        else if(line[0] == '\t' && ncommands > 0) {
            add_syscall_to_command(line);
        }
        else {
            printf("ERROR - line %i of '%s' is not recognized\n", lc, filename);
            exit(EXIT_FAILURE);
        }
    }
    fclose(fp);
    patch_commands();
}

//  NOT REQUIRED, BUT PROVIDES A CHECK THAT THINGS HAVE BEEN STORED CORRECTLY
void dump_commands(void)
{
    FOREACH_COMMAND {
        printf("%s\n", commands[c].name);

        for(int s=0 ; s<commands[c].nsyscalls ; ++s) {
            switch (commands[c].syscalls[s].which) {
            case SYS_SPAWN:
                printf("\t%i\t%s\t%s\n", 
                    commands[c].syscalls[s].when,
                    syscalls[commands[c].syscalls[s].which] ,
                    commands[commands[c].syscalls[s].arg0].name );
                break;

            case SYS_READ:
            case SYS_WRITE:
                printf("\t%i\t%s\t%s\t%i\n", 
                    commands[c].syscalls[s].when,
                    syscalls[commands[c].syscalls[s].which] ,
                    devices[commands[c].syscalls[s].arg0].name,
                    commands[c].syscalls[s].arg1    );
                break;

            case SYS_SLEEP:
                printf("\t%i\t%s\t%i\n", 
                    commands[c].syscalls[s].when,
                    syscalls[commands[c].syscalls[s].which],
                    commands[c].syscalls[s].arg0    );
                break;

            case SYS_WAIT:
            case SYS_EXIT:
                printf("\t%i\t%s\n", 
                    commands[c].syscalls[s].when,
                    syscalls[commands[c].syscalls[s].which] );
                break;
            }
        }
    }
}

//  ----------------------------------------------------------------------

//  AN ARRAY AND FUNCTIONS TO MANAGE THE SYSTEM'S WAITING QUEUE

int WAITING_queue[MAX_RUNNING_PROCESSES];       // indicies into processes[]

int nwaiting                = 0;
#define FOREACH_WAITING     for(int w=0 ; w<nwaiting ; ++w)

void init_WAITING_queue(void)
{
    nwaiting    = 0;
}

void append_to_WAITING_queue(int proc_on_CPU)
{
    DEBUG("wait, pid%i.RUNNING->WAITING", processes[proc_on_CPU].pid);
    flush_DEBUG(processes[proc_on_CPU].command);

    WAITING_queue[nwaiting]         = proc_on_CPU;
    processes[proc_on_CPU].state    = STATE_WAITING;
    ++nwaiting;
}

void unblock_WAITING(void)
{
    FOREACH_WAITING {
        int proc   = WAITING_queue[w];

        if(processes[proc].state == STATE_WAITING && processes[proc].nchildren == 0) {
            append_to_READY_queue(proc, "WAITING");
            advance_time(TIME_CORE_STATE_TRANSITIONS);

            for(int t=w ; t<(nwaiting-1) ; ++t) {       // 'slide' all left by 1
                WAITING_queue[t]   = WAITING_queue[t+1];
            }
            --nwaiting;
            --w;
        }
    }
}

//  ----------------------------------------------------------------------

//  AN ARRAY AND FUNCTIONS TO MANAGE THE SYSTEM'S SLEEPING QUEUE

struct {
    int         proc;                   // index into processes[]
    int         until;                  // when the process awakens
} SLEEPING_queue[MAX_RUNNING_PROCESSES];

int nsleeping               = 0;
#define FOREACH_SLEEPER     for(int s=0 ; s<nsleeping ; ++s)

void init_SLEEPING_queue(void)
{
    nsleeping   = 0;
}

void append_to_SLEEPING_queue(int proc_on_CPU, int duration)
{
    DEBUG("sleep %i, pid%i.RUNNING->SLEEPING", duration, processes[proc_on_CPU].pid);

    processes[proc_on_CPU].state        = STATE_SLEEPING;
    SLEEPING_queue[nsleeping].proc      = proc_on_CPU;
//  NOTE WE'RE STORING THE TIME THE PROCESS WAKES UP, NOT JUST THE SLEEPING TIME
    SLEEPING_queue[nsleeping].until     = USECS_SINCE_REBOOT+duration+1;
    ++nsleeping;
}

void unblock_SLEEPING(void)
{
    int ORIG_USECS  = USECS_SINCE_REBOOT;

    FOREACH_SLEEPER {
        if(SLEEPING_queue[s].until <= ORIG_USECS) {

            append_to_READY_queue(SLEEPING_queue[s].proc, "SLEEPING");
            advance_time(TIME_CORE_STATE_TRANSITIONS);

            for(int t=s ; t<(nsleeping-1) ; ++t) {      // 'slide' all left by 1
                SLEEPING_queue[t]   = SLEEPING_queue[t+1];
            }
            --nsleeping;
            --s;
        }
    }
}

//  ----------------------------------------------------------------------

#include <time.h>           // only used to report real-world rebooting time

int execute_commands(int first)
{
    int proc_on_CPU         = UNKNOWN;
    int total_time_on_CPU   = 0;
    int timequantum_expires = UNKNOWN;

    USECS_SINCE_REBOOT      = 0;

//  THESE 4 LINES NOT PART OF THE PROJECT, JUST USED TO REPORT REAL-WORLD TIME
    time_t      now;
    time(&now);
    char *t = ctime(&now);
    t[19]   = '\0';

    DEBUG("REBOOTING at %s, with timequantum=%i", t, timequantum);
    flush_DEBUG(UNKNOWN);
    spawn_process(first, UNKNOWN);
    flush_DEBUG(UNKNOWN);

    USECS_SINCE_REBOOT      = -1;   // not a mistake

//  EXECUTE UNTIL THE LAST PROCESS HAS EXITED
    while(nprocesses > 0) {
        advance_time(1);

//  IS A PROCESS RUNNING ON THE CPU?
        if(proc_on_CPU != UNKNOWN) {
            int c       = processes[proc_on_CPU].command;
            int s       = processes[proc_on_CPU].next_syscall;

//  THE RUNNING PROCESS ISSUES A SYSTEM-CALL, IT WILL LOSE THE CPU
            if(processes[proc_on_CPU].time_on_CPU == commands[c].syscalls[s].when) {
                int which       = commands[c].syscalls[s].which;
                ++processes[proc_on_CPU].next_syscall;

                switch (which) {
                    case SYS_SPAWN:
                        spawn_process(commands[c].syscalls[s].arg0, processes[proc_on_CPU].pid);
                        ++processes[proc_on_CPU].nchildren;
                        append_to_READY_queue(proc_on_CPU, "RUNNING");
                        advance_time(TIME_CORE_STATE_TRANSITIONS);
                        break;

                    case SYS_READ:
                    case SYS_WRITE:
                        append_to_IO_BLOCKED_queue(proc_on_CPU, which,
                            commands[c].syscalls[s].arg0, commands[c].syscalls[s].arg1);
                        processes[proc_on_CPU].state    = STATE_IO_BLOCKED;
                        break;

                    case SYS_SLEEP:
                        append_to_SLEEPING_queue(proc_on_CPU, commands[c].syscalls[s].arg0);
                        advance_time(TIME_CORE_STATE_TRANSITIONS);
                        break;

                    case SYS_WAIT:
                        if(processes[proc_on_CPU].nchildren == 0) {
                            DEBUG("wait (but no child processes)");
                            append_to_READY_queue(proc_on_CPU, "RUNNING");
                            flush_DEBUG(processes[proc_on_CPU].command);
                        }
                        else {
                            append_to_WAITING_queue(proc_on_CPU);
                        }
                        advance_time(TIME_CORE_STATE_TRANSITIONS);
                        break;

                    case SYS_EXIT:
                        total_time_on_CPU   += processes[proc_on_CPU].time_on_CPU;
                        exit_process(proc_on_CPU);
                        break;

                    default:
                        printf("ERROR - unknown syscall %i\n", which);
                        exit(EXIT_FAILURE);
                        break;
                }
//  EACH SYSTEM-CALL HAS RESULTED IN ITS PROCESS LEAVING THE CPU
                proc_on_CPU     = UNKNOWN;
                flush_DEBUG(UNKNOWN);
            }

//  IF A PROCESS IS ON THE CPU...
            if(proc_on_CPU != UNKNOWN) {

//  PROCESS ON CPU HAS CONSUMED SOME CPU (COMPUTATION) TIME
                ++processes[proc_on_CPU].time_on_CPU;
                if(verbose) {
                    DEBUG("c"); flush_DEBUG(proc_on_CPU);
                }

//  HAS THE RUNNING PROCESS'S TIME QUANTUM EXPIRED?
                if(USECS_SINCE_REBOOT >= timequantum_expires) {
                    DEBUG("timequantum expired");
                    append_to_READY_queue(proc_on_CPU, "RUNNING");
                    proc_on_CPU = UNKNOWN;
                    advance_time(TIME_CORE_STATE_TRANSITIONS);
                }
            }
        }

//  IF CPU IS NOW IDLE AND PROCESSES REMAIN....
        if(proc_on_CPU == UNKNOWN && nprocesses > 0) {
            unblock_SLEEPING();
            unblock_WAITING();
            unblock_completed_IO();
            start_pending_IO();

//  IDLE CPU CAN RECEIVE THE FIRST READY PROCESS
            if(nready > 0) {
                proc_on_CPU         = dequeue_READY_queue();
                timequantum_expires = USECS_SINCE_REBOOT + timequantum; // new TQ

                DEBUG("pid%i now on CPU, gets new timequantum", processes[proc_on_CPU].pid);
                flush_DEBUG(proc_on_CPU);
            }

//  STILL IDLE?
            if(proc_on_CPU == UNKNOWN && verbose) {
                DEBUG("idle"); flush_DEBUG(UNKNOWN);
            }
        }
    }                                   // while(nprocesses > 0)

//  WE HAVE FINISHED!
    DEBUG("nprocesses=0, SHUTDOWN");
    flush_DEBUG(UNKNOWN);

    return total_time_on_CPU;
}

//  ----------------------------------------------------------------------

int main(int argc, char *argv[])
{
//  ENSURE THAT WE HAVE THE CORRECT NUMBER OF COMMAND-LINE ARGUMENTS
    if(argc != 3) {
        printf("Usage: %s sysconfig-file command-file\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    verbose = (getenv("VERBOSE") != NULL);      // debug printing required?

//  READ THE SYSTEM CONFIGURATION FILE
    read_sysconfig(argv[0], argv[1]);
//  NOT REQUIRED, BUT PROVIDES A CHECK THAT THINGS HAVE BEEN STORED CORRECTLY
//  dump_sysconfig();

//  READ THE COMMAND FILE
    read_commands(argv[0], argv[2]);
//  NOT REQUIRED, BUT PROVIDES A CHECK THAT THINGS HAVE BEEN STORED CORRECTLY
//  dump_commands();

    init_processes();
    init_READY_queue();
    init_SLEEPING_queue();
    init_WAITING_queue();
    init_devices_and_IO_BLOCKED_queues();

//  EXECUTE COMMANDS, STARTING AT FIRST IN command-file, UNTIL NONE REMAIN
    int total_time_on_CPU   = execute_commands(0);  // first spawn commands[0]

//  PRINT THE PROGRAM'S RESULTS
    DEBUG("%iusecs total system time, %iusecs onCPU by all processes, %i/%i -> %i%%",
            USECS_SINCE_REBOOT, total_time_on_CPU,
            total_time_on_CPU, USECS_SINCE_REBOOT,
            100*total_time_on_CPU / USECS_SINCE_REBOOT);
    flush_DEBUG(UNKNOWN);

    printf("measurements  %i  %i\n", USECS_SINCE_REBOOT, 100*total_time_on_CPU / USECS_SINCE_REBOOT);

    exit(EXIT_SUCCESS);
}

//  vim: ts=8 sw=4