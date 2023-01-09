//
// Created by radio on 06.01.2023.
//

#include <fcntl.h>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <queue>
#include <semaphore.h>
#include <sstream>
#include <string>s
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#define RUN 1
#define OUT 2
#define ERR 3
#define KILL 4
#define QUIT 5
#define SLEEP 6


using namespace std;
bool debug = false;
unordered_map<int, string> tasks_out;
std::mutex tasks_mutex_out;
unordered_map<int, string> tasks_err;
std::mutex tasks_mutex_err;
unordered_map<int, int> tasks_kill;
std::mutex tasks_mutex_kill;
mutex tasks_mutex;

bool incommand;
mutex incommand_mutex;

string g_data;
deque<string> task_messages;
sem_t one_command_semaphore;

template <typename T>
class BlockingQueue
{
public:
    BlockingQueue()
        :mtx(), full_(), empty_() { }

    void push(const T& data){
        std::unique_lock<std::mutex> lock(mtx);
        queue_.push(data);
        empty_.notify_all();
    }

    T pop(){
        std::unique_lock<std::mutex> lock(mtx);
        while(queue_.empty()){
            empty_.wait(lock );
        }

        assert(!queue_.empty());
        T front(queue_.front());
        queue_.pop();
        full_.notify_all();
        return front;
    }

private:
    mutable std::mutex mtx;
    std::condition_variable full_;
    std::condition_variable empty_;
    std::queue<T> queue_;
};

bool check(const string& data){
    istringstream my_stream(data);
    string token;
    my_stream >> token;
    if(token[0]=='q' || token[0] == EOF)return true;
    return false;
}

void producer(BlockingQueue<string>* commands) {
    while (getline(cin, g_data)) {
        commands->push(g_data);
        if(check(g_data))break;
    }
    //commands.release();
    commands->push("quit");
}

void readpipe(int fd[], int index, bool is_out){
    if (close(fd[1]) == -1) exit(1);
    char smth;
    string out;
    while(read(fd[0],&smth, sizeof(char))>0){
        if (smth == '\n') {
            if (is_out) {
                tasks_mutex_out.lock();
                tasks_out[index] = out;
                tasks_mutex_out.unlock();
            }
            else {
                tasks_mutex_err.lock();
                tasks_err[index] = out;
                tasks_mutex_err.unlock();
            }
            out.clear();
        }
        else out+= smth;
    }
    if (close(fd[0]) == -1) exit(1);
}

void run(vector<string> argv, int index) { //function for run command
    int fdout[2];
    int fderr[2];
    if (pipe2(fdout, O_CLOEXEC) == -1) exit(1);
    if (pipe2(fderr, O_CLOEXEC) == -1) exit(1);

    int pid = fork();
    if (pid == -1) exit(1);

    if (pid == 0) { //child process for execvp
        if (close(fdout[0]) == -1) exit(1);
        if (close(fderr[0]) == -1) exit(1);

        if (dup2(fdout[1], STDOUT_FILENO) == -1) exit(1);
        if (dup2(fderr[1], STDERR_FILENO) == -1) exit(1);

        if (close(fdout[1]) == -1) exit(1);
        if (close(fderr[1]) == -1) exit(1);

        char * args[argv.size()+1];
        int i;
        for(i = 0; i < argv.size()-1; i++){
            args[i] = const_cast<char*>(argv.at(i+1).c_str());
        }
        args[i] = nullptr;
        execvp(args[0], args);
        cerr << "command didnt work" << endl;
        exit(1);
    }
    thread readout(readpipe, fdout, index, true);
    thread readerr(readpipe, fderr, index, false);

    tasks_mutex_kill.lock();
    tasks_kill[index] = pid;
    tasks_mutex_kill.unlock();

    sem_post(&one_command_semaphore);

    readout.join();
    readerr.join();

    int status = 0;
    wait(&status);
    string message;

    if (WIFEXITED(status)) {
        message +="Task " + to_string(index) + " ended: status " + to_string(WEXITSTATUS(status)) + '.';
    } else {
        message +="Task " + to_string(index) + " ended: signalled.";
    }

    incommand_mutex.lock();
    if (incommand) task_messages.push_back(message);
    else cout << message<< endl;
    incommand_mutex.unlock();
}


void producerThread(BlockingQueue<string>* commands) { producer(commands); }

void task_kill(int T, bool is_quit){
    tasks_mutex_kill.lock();
    if (tasks_kill[T] != 0) {
        if (!is_quit) kill(tasks_kill[T], SIGINT);
        else kill(tasks_kill[T], SIGKILL);
    }
    tasks_mutex_kill.unlock();
}


int main() {

    BlockingQueue<string> commands;
    sem_init(&one_command_semaphore, 0, 0);
    mutex thread_creation;

    int index = 0;
    int switch_;
    int T = -1;
    std::thread exec_input(producerThread,&commands); // thread to read input
    vector<string> token;
    vector<thread> threads;

    while (true) { //main process loop
        
        //checking for awaiting messages for executor to print out
        incommand_mutex.lock();
        incommand = false;
        while(!task_messages.empty()){

            cout << task_messages.front()<< endl;
            task_messages.pop_front();
        }
        incommand_mutex.unlock();
        
        //await new command on blocking queue
        string command = commands.pop(); 
 
        incommand_mutex.lock();
        incommand = true;
        incommand_mutex.unlock();
        
        istringstream my_stream(command);
        token.clear();
        string tmp;
        switch_ = 0;
        
        // Traverse till stream is valid
        while (my_stream >> tmp) {
            if (tmp.empty()) continue;
            token.push_back(tmp);
            if (switch_ == 0) { //check the command
                if (token[0][0] == 'q') {
                    switch_ = QUIT;
                    break;
                }
                else if (token[0][0] == 'r') switch_ = RUN;
                else if (token[0][0] == 'o') switch_ = OUT;
                else if (token[0][0] == 'e') switch_ = ERR;
                else if (token[0][0] == 'k') switch_ = KILL;
                else if (token[0][0] == 's') switch_ = SLEEP;
            }
            else if (switch_ != RUN) T = stoi(token[1]);
        }
        
        if (switch_ == QUIT) {
            for(int j = 0; j < index; j++){
                task_kill(j, true);
            }
            break;
        }
        
        switch (switch_) {
        case RUN:
            thread_creation.lock();
            threads.emplace_back(run, token, index);
            thread_creation.unlock();

            sem_wait(&one_command_semaphore);

            tasks_mutex_kill.lock();
            cout << "Task "<<index<<" started: pid " << tasks_kill[index] << '.' << endl;
            tasks_mutex_kill.unlock();

            index++;
            break;
        case OUT:
            tasks_mutex_out.lock();
            cout <<"Task " << T << " stdout: \'" << tasks_out[T] << "\'."<< endl;
            tasks_mutex_out.unlock();
            break;
        case ERR:
            tasks_mutex_err.lock();
            cout <<"Task " << T << " stderr: \'" << tasks_err[T] << "\'."<< endl;
            tasks_mutex_err.unlock();
            break;
        case KILL:
            task_kill(T, false);
            break;
        case SLEEP:
            usleep(T * 1000);
            break;
        }
    }
    incommand_mutex.lock();
    incommand = false;
    while(!task_messages.empty()){
        cout << task_messages.front()<< endl;
        task_messages.pop_front();
    }
    incommand_mutex.unlock();


    //joining all task threads

    tasks_mutex.lock();
    for (auto &thread : threads) {
        thread.join();
    }
    tasks_mutex.unlock();

    exec_input.join();

    if (sem_destroy(&one_command_semaphore) == -1) exit(1);
    return 0;
}
