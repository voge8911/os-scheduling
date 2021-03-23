/* to run program:
    ./bin/osscheduler resrc/config_01.txt
*/
#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    while (!(shared_data->all_terminated))
    {
        int counter = 0;
        // Clear output from previous iteration
        clearOutput(num_lines);

        for (int i = 0; i < processes.size(); i++)
        {   
            // Get current time
            uint64_t current_time = currentTime();
            // *Check if any processes need to move from NotStarted to Ready (based on elapsed time) 
            // If so put that process in the ready queue
            if (current_time - start >= (processes[i]->getStartTime())
                && processes[i]->getState() == Process::State::NotStarted)
            {
                processes[i]->setState(Process::State::Ready, currentTime());
                {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(processes[i]);
                }
            }
            // *Check if any processes have finished I/O burst
            // If so put the process back in the ready queue
            if (processes[i]->getState() == Process::State::IO && 
                (currentTime() - processes[i]->getBurstStartTime()) > processes[i]->getBurstTime(processes[i]->getCurrentBurst()))
            {
                processes[i]->setState(Process::State::Ready, currentTime());
                //processes[i]->updateProcess(currentTime());
                processes[i]->nextBurst();
                {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(processes[i]);
                }
            } 

            // *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
            // RR time slice expires
            if (config->algorithm == ScheduleAlgorithm::RR)
            {
                if (processes[i]->getState() == Process::State::Running 
                    && processes[i]->getCpuTime() > config->time_slice)
                {
                    processes[i]->interrupt();
                }
            }
            // newly ready process has higher priority
            // double check this if statment
            //if (processes[i]->getState() == Process::State::Running 
            //    && processes[i]->getPriority() > shared_data->ready_queue.front()->getPriority())
            {
                //processes[i]->interrupt();
            }

            // *Sort the ready queue (if needed - based on scheduling algorithm)
            if (shared_data->algorithm == ScheduleAlgorithm::SJF)
            {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                shared_data->ready_queue.sort(SjfComparator());
            }
            if (shared_data->algorithm == ScheduleAlgorithm::PP)
            {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                shared_data->ready_queue.sort(PpComparator());
            }
            
        }
        // *Determine if all processes are in the terminated state
        for (int i = 0; i < processes.size(); i++)
        {
            
            if (processes[i]->getState() == Process::State::Terminated)
            {
                counter++;
            }
        }
        if (counter == processes.size())
        {
            shared_data->all_terminated == true;
        }
        // Do the following:
        //   - Get current time
        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        //   - *Sort the ready queue (if needed - based on scheduling algorithm)
        //   - *Determne if all processes are in the terminated state
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);

        // sleep 50 ms
        usleep(50000);
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics
    //printf("Final Statistics: ");
    //  - CPU utilization
    //printf("CPU Utilization: ");
    //  - Throughput
    //     - Average for first 50% of processes finished
    //printf("Throughput average of first 50% of processes finished: ");
    //     - Average for second 50% of processes finished
    //printf("Throughput average of second 50% of processes finished: ");
    //     - Overall average
    //printf("Overall Throughput Average: ");
    //  - Average turnaround time
    //printf("Average turnaround time: ");
    //  - Average waiting time
    //printf("Average waiting time: ");


    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    while (!(shared_data->all_terminated))
    { 
        // *Get process at front of ready queue
        std::list<Process *>::iterator it;
        Process *p;
        {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            it = shared_data->ready_queue.begin();
            p = *it;
            if (shared_data->ready_queue.size() < 1)
            {
                continue;
            }
            shared_data->ready_queue.erase(it);
        }
        p->setState(Process::State::Running, currentTime());
        p->setCpuCore(core_id);
        p->setBurstStartTime(currentTime());
        while (1)
        {
            // Simulate the processes running
            // delay 50 ms
            usleep(50000);
            p->updateProcess(currentTime());
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            // Stop running process if...
            // CPU burst time has elapsed
            //printf("burst time for %d is %d", core_id, p->getBurstTime(p->getCurrentBurst()));
            if ((currentTime() - p->getBurstStartTime()) >= p->getBurstTime(p->getCurrentBurst())) 
            {
                // go to next burst
                p->nextBurst();
                p->setBurstStartTime(currentTime());
                p->setCpuCore(-1);
                // Place the process back in I/O queue if CPU burst finished (and process not finished)
                // Or Terminated if CPU burst finished and no more bursts remain
                //printf("current burst is %d and NumBursts is %d", p->getCurrentBurst(), p->getNumBursts());
                if ((p->getCurrentBurst() <= p->getNumBursts()) && p->getRemainingTime() > 0.0)
                {
                    usleep(50000);
                    p->setState(Process::State::IO, currentTime());
                }
                else
                {
                    p->setState(Process::State::Terminated, currentTime());
                    p->setRemainingTime(0);
                }
                break;
            }
            // Or interrupted (RR time slice has elapsed or process preempted by higher priority process)
          /*else if (((shared_data->algorithm == ScheduleAlgorithm::RR) && p->getCpuTime() > shared_data->time_slice) ||
                     p->getPriority() > shared_data->ready_queue.front()->getPriority())
            {
                // Place the process back in *Ready queue if interrupted
                // (be sure to modify the CPU burst time to now reflect the remaining time)
                p->setState(Process::State::Ready, currentTime());
                shared_data->ready_queue.push_front(p);
                p->updateBurstTime(p->getCurrentBurst(), p->getBurstTime(p->getCurrentBurst()) - p->getCpuTime());
                p->setCpuCore(-1);
                
                
                break;
            }*/
        }
        //  - Wait context switching time
        usleep(shared_data->context_switch);
        //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    }
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
