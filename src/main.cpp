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
typedef struct SchedulerData
{
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process *> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process *> &processes, std::mutex &mutex);
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
    std::vector<Process *> processes;

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
            if (current_time - start >= (processes[i]->getStartTime()) && processes[i]->getState() == Process::State::NotStarted)
            { // If so put that process in the ready queue
                processes[i]->setState(Process::State::Ready, currentTime());
                {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(processes[i]);
                }
            }
            //processes[i]->updateProcess(currentTime());
            // *Check if any processes have finished I/O burst
            if (processes[i]->getState() == Process::State::IO &&
                (currentTime() - processes[i]->getBurstStartTime()) > processes[i]->getBurstTime(processes[i]->getCurrentBurst()))
            { // If so put the process back in the ready queue
                processes[i]->setState(Process::State::Ready, currentTime());
                processes[i]->nextBurst();
                {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(processes[i]);
                }
            }
            // *Check if any running process need to be interrupted
            {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                if ((shared_data->algorithm == ScheduleAlgorithm::RR &&
                     // RR time slice as expires
                     (currentTime() - processes[i]->getBurstStartTime() >= shared_data->time_slice)) ||
                    // Newly ready process has higher priority
                    (!shared_data->ready_queue.empty() && processes[i]->getPriority() > shared_data->ready_queue.front()->getPriority()))
                {
                    processes[i]->interrupt();
                }
                // *Sort the ready queue (if needed - based on scheduling algorithm)
                if (shared_data->ready_queue.size() > 1)
                {
                    if (shared_data->algorithm == ScheduleAlgorithm::SJF)
                    {
                        shared_data->ready_queue.sort(SjfComparator());
                    }
                    if (shared_data->algorithm == ScheduleAlgorithm::PP)
                    {
                        shared_data->ready_queue.sort(PpComparator());
                    }
                }
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
            shared_data->all_terminated = true;
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

    double total_wait_time = 0;
    double total_cpu_time = 0;
    double total_turnaround_time = 0;
    double cpu_utilization = 0;
    double num_processes = 0;
    int midpoint = 0;
    int second_half_midpoint = 0;
    std::vector<double> turnaround_times;
    
    for (int i = 0; i < processes.size(); i++)
    {
        total_wait_time += processes[i]->getWaitTime();
        total_cpu_time += processes[i]->getCpuTime();
        total_turnaround_time += processes[i]->getTurnaroundTime();
        turnaround_times.push_back(processes[i]->getTurnaroundTime());
        num_processes++;
    }
    midpoint = (int)(num_processes/2);
    second_half_midpoint = num_processes - midpoint;
    std::sort(turnaround_times.begin(), turnaround_times.end());
    cpu_utilization = (total_cpu_time / total_turnaround_time) * 100.0;

    // print final statistics
    printf("\n Final Statistics: \n"); 
    printf("-------------------------------------------------- \n");
    //  - CPU utilization
    printf(" - CPU Utilization: %0.1lf%% \n", cpu_utilization);
    //  - Throughput
    //     - Average for first 50% of processes finished
    printf(" - Throughput average of first 50%% of processes finished: %0.3lf processes/second\n", midpoint / turnaround_times[midpoint-1]);
    //     - Average for second 50% of processes finished
    printf(" - Throughput average of second 50%% of processes finished: %0.3lf processes/second\n", second_half_midpoint / turnaround_times[num_processes-1]);
    //     - Overall average
    printf(" - Overall Throughput Average: %0.3lf processes/second\n", num_processes / turnaround_times[num_processes-1]);
    //printf(" - Max Turnaround Time Check = %0.1lf\n", turnaround_times[num_processes-1]);
    //  - Average turnaround time
    printf(" - Average Turnaround Time: %0.1lf seconds\n", total_turnaround_time / num_processes);
    //  - Average waiting time
    printf(" - Average Waiting Time: %0.1lf seconds\n", total_wait_time / num_processes);

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
        uint64_t time_since_burst_started = 0;
        while (1)
        {
            // Simulate the processes running
            // delay 50 ms
            usleep(50000);
            p->updateProcess(currentTime());
            time_since_burst_started = currentTime() - p->getBurstStartTime();
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            // Stop running process if...
            // CPU burst time has elapsed
            if (time_since_burst_started >= p->getBurstTime(p->getCurrentBurst()))
            {
                // go to next burst
                p->nextBurst();
                p->setBurstStartTime(currentTime());
                p->setCpuCore(-1);
                // Place the process back in I/O queue if CPU burst finished (and process not finished)
                if ((p->getCurrentBurst() <= p->getNumBursts()) && p->getRemainingTime() > 0.0)
                {
                    usleep(50000);
                    p->setState(Process::State::IO, currentTime());
                }
                // Terminated if CPU burst finished and no more bursts remain
                else
                {
                    p->setState(Process::State::Terminated, currentTime());
                    p->setRemainingTime(0);
                }
                break;
            }
            // Or interrupted
            else if (p->isInterrupted())
            {
                // Modify the CPU burst time to now reflect the remaining time
                p->updateBurstTime(p->getCurrentBurst(), p->getBurstTime(p->getCurrentBurst()) - time_since_burst_started);
                // Place the process back in *Ready queue if interrupted
                p->setState(Process::State::Ready, currentTime());
                shared_data->ready_queue.push_back(p);
                p->setCpuCore(-1);
                p->interruptHandled();
                break;
            }
        }
        //  - Wait context switching time
        usleep(shared_data->context_switch);
        //  - * = accesses shared data (ready queue), use proper synchronization
    }
}

int printProcessOutput(std::vector<Process *> &processes, std::mutex &mutex)
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
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
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
