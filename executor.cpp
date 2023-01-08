//
// Created by radio on 06.01.2023.
//
#include <chrono>
#include <condition_variable> // std::condition_variale
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
using namespace std;
bool debug = false;
std::mutex g_mutex;
std::condition_variable g_cv;
unordered_map<int, string> tasks_out;
std::mutex tasks_mutex_out;
unordered_map<int, string> tasks_err;
std::mutex tasks_mutex_err;
unordered_map<int, int> tasks_kill;
std::mutex tasks_mutex_kill;
bool incommand;
mutex incommand_mutex;
string g_data;
deque<string> task_messages;

template <typename T>
class bqueue
{
private:
    std::mutex              d_mutex;
    std::condition_variable d_condition;
    std::deque<T>           d_queue;
public:
    void push(T const& value) {
        {
            std::unique_lock<std::mutex> lock(this->d_mutex);
            d_queue.push_front(value);
        }
        this->d_condition.notify_one();
    }
    T pop() {
        std::unique_lock<std::mutex> lock(this->d_mutex);
        this->d_condition.wait(lock, [=]{ return !this->d_queue.empty(); });
        T rc(std::move(this->d_queue.back()));
        this->d_queue.pop_back();
        return rc;
    }
    void release() {
        this->d_condition.notify_one();
    }
};
bqueue<string> commands;
bool check(string data){
    istringstream my_stream(data);
    string token;
    my_stream >> token;
    if(token[0]=='q')return true;
    return false;
}

void producer() {
    while (getline(cin, g_data)) {
        commands.push(g_data);
        if(check(g_data))break;
    }
    //commands.release();
    commands.push("quit");
}

void readpipe(int fd[], int index, bool is_out){

    close(fd[1]);
    char smth;
    string out;
    int count = 0;
    if(debug ) cout << "started reading pipe " << index << " is_out: "<< is_out <<  endl;
    while(read(fd[0],&smth, sizeof(char))>0){
        if (smth == '\n') {
            //out+= '\0';
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
            if (debug) cout << "from task " << index << " saved line: " << out << endl;
            out.clear();

            count = 0;

        }
        else out+= smth;
    }
    if(debug) cout << "stopped reading pipe " << index << endl;
    close(fd[0]);
}

void run(vector<string> argv, int index) { //function for run command
    //sleep(5);
    //cout << "run started for index: " << index << endl;
    int fdout[2];
    int fderr[2];
    pipe(fdout);
    pipe(fderr);

    int pid = fork();
    if (pid == -1) exit(1);

    if (pid == 0) { //child process for execvp
        cout << "Task "<<index<<" started: pid " << getpid() << '.' << endl;
        close(fdout[0]);
        close(fderr[0]);

        dup2(fdout[1], STDOUT_FILENO);
        dup2(fderr[1], STDERR_FILENO);

        close(fdout[1]);
        close(fderr[1]);

        char * args[argv.size()+1];
        int i;
        for(i = 0; i < argv.size()-1; i++){
            args[i] = const_cast<char*>(argv.at(i+1).c_str());
            if(debug)cout << "args for exec are:" << args[i] << endl;
        }
        args[i] = nullptr;
        args[i+2] = nullptr;


        execvp(args[0], args);
        cout << "command didnt work" << endl;
    }
    thread readout(readpipe, fdout, index, true);
    thread readerr(readpipe, fderr, index, false);
    tasks_mutex_kill.lock();
    tasks_kill[index] = pid;
    tasks_mutex_kill.unlock();
    readout.join();
    readerr.join();
    int status = 0;
    wait(&status);
    string message;
    if(WIFEXITED(status)){
        message +="Task " + to_string(index) + " ended: status " + to_string(status) + '.';
    } else {
        message +="Task " + to_string(index) + " ended: signalled.";
    }
    incommand_mutex.lock();
    if(incommand){

        task_messages.push_back(message);
        //cout << "hello" << "Task " + to_string(index) + " ended: status " + to_string(status) + '.'<< endl;
    }
    else{
        cout << message<< endl;
    }
    incommand_mutex.unlock();


    int i = 0;
//    while(args[i]!=nullptr){
//        delete[] args[i++];
//    }
}


void producerThread() { producer(); }

void task_kill(int T){
    tasks_mutex_kill.lock();
    if (tasks_kill[T] != 0) kill(tasks_kill[T], SIGINT);
    tasks_mutex_kill.unlock();
}
int main() {

    int index = 0;
    std::vector<std::thread> threads; // threads for new tasks
    std::thread exec_input(producerThread); // thread to read input


    while(true){ //main process loop

        incommand_mutex.lock();
        incommand = false;
        while(!task_messages.empty()){

            cout << task_messages.front()<< endl;
            task_messages.pop_front();
        }
        incommand_mutex.unlock();
        string command = commands.pop(); //pop command from input queue
        incommand_mutex.lock();
        incommand = true;
        incommand_mutex.unlock();
        if (debug) cout << "The value read: " << command << endl;
        if(command.empty())continue;
        char* arguments[512];
        char** next = arguments;
        int c = 0;
        int switch_ = 0;
        int T = -1;

        string delimiter = " ";

        istringstream my_stream(command);


        // To store the stream string


        size_t pos = -1;
        int i =0;
        vector<string> token;
        token.clear();
        string tmp;
        // Traverse till stream is valid
        while (my_stream >> tmp) {
            token.push_back(tmp);
            i++;
            if (switch_ == 0) { //check the command
                if (token[0][0] == 'q') {
                    switch_ = -1;
                    break;
                }
                else if (token[0][0] == 'r') switch_ = 1;
                else if (token[0][0] == 'o') switch_ = 2;
                else if (token[0][0] == 'e') switch_ = 3;
                else if (token[0][0] == 'k') switch_ = 4;
                else if (token[0][0] == 's') switch_ = 5;
            }
            else { //if command already determined - process arguments
                if (switch_ == 1) {
                    int x = 0;
                } else {
                    T = stoi(token[i-1]);
                }
            }


        }
        if (switch_ == -1) {
            for(int j = 0; j < index; j++){
                task_kill(j);
            }
            break;
        }
        if (switch_ == 1) {

            threads.emplace_back(run, token, index++);
        }
        else if (switch_ == 2) {
            tasks_mutex_out.lock();
            cout <<"Task " << T << " stdout: \'" << tasks_out[T] << "\'."<< endl;
            tasks_mutex_out.unlock();

        }
        else if (switch_ == 3) {
            tasks_mutex_err.lock();
            cout <<"Task " << T << " stderr: \'" << tasks_err[T] << "\'."<< endl;
            tasks_mutex_err.unlock();
        }
        else if (switch_ == 4) {
            task_kill(T);

        }
        else if (switch_ == 5) {
            usleep(T * 1000);
        }
    }
    incommand_mutex.lock();
    incommand = false;
    while(!task_messages.empty()){

        cout << task_messages.front()<< endl;
        task_messages.pop_front();
    }
    incommand_mutex.unlock();

    exec_input.join();
    for( auto &thread : threads){
        thread.join();
    }
    if(debug)cout << "Executor finished" << endl;
    return 0;
}

//TODO: implement proper handling of quit command
//TODO: move 'Task' messages handling to the main process
//TODO: implement executor waiting for new thread to be created