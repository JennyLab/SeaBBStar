#!/usr/bin/env python
"""
The proxy program for CS5450  -Tingwei.
"""

import sys
import subprocess
import time
from threading import Thread, Lock
from socket import SOCK_STREAM, socket, AF_INET

address = 'localhost'
threads = {}

msgs = {}


wait_chat_log = False

started_processes = {}

debug = False

class ClientHandler(Thread):
    def __init__(self, index, address, port):
        Thread.__init__(self)
        self.daemon = True
        self.index = index
        self.sock = socket(AF_INET, SOCK_STREAM)
        self.sock.connect((address, port))
        self.buffer = ""
        self.valid = True

    def run(self):
        global threads, wait_chat_log
        while self.valid:
            if "\n" in self.buffer:
                (l, rest) = self.buffer.split("\n", 1)
                self.buffer = rest
                s = l.split()
                if len(s) < 2:
                    continue
                if s[0] == 'chatLog':
                    chatLog = s[1]
                    print(chatLog)
                    wait_chat_log = False
                else:
                    print( 'WRONG MESSAGE:', s)
            else:
                try:
                    data = self.sock.recv(1024)
                    #sys.stderr.write(data)
                    self.buffer += data
                except:
                    #print sys.exc_info()
                    self.valid = False
                    del threads[self.index]
                    self.sock.close()
                    break

    def send(self, s):
        if self.valid:
            self.sock.send((str(s) + '\n').encode('utf-8'))

    def close(self):
        try:
            self.valid = False
            self.sock.close()
        except:
            pass

def send(index, data, set_wait=False):
    global threads, wait_chat_log
    while wait_chat_log:
        time.sleep(0.01)
    pid = int(index)
    if set_wait:
        wait_chat_log = True
    threads[pid].send(data)

def exit(exit=False):
    global threads, wait_chat_log

    wait = wait_chat_log
    wait = wait and (not exit)
    while wait:
        time.sleep(0.01)
        wait = wait_chat_log

    time.sleep(2)
    for k in threads:
        threads[k].close()
    subprocess.Popen(['./stopall'], stdout=open('/dev/null'), stderr=open('/dev/null'))
    sys.stdout.flush()
    time.sleep(0.1)
    sys.exit(0)

def timeout():
    time.sleep(120)
    exit(True)

# def main():
#     global threads, wait_chat_log, started_processes, debug
#     timeout_thread = Thread(target = timeout, args = ())
#     timeout_thread.daemon = True
#     timeout_thread.start()

#     while True:
#         line = ''
#         try:
#             line = sys.stdin.readline()
#         except: # keyboard exception, such as Ctrl+C/D
#             exit(True)
#         if line == '': # end of a file
#             exit()
#         line = line.strip() # remove trailing '\n'
#         if line == 'exit': # exit when reading 'exit' command
#             exit()
#         sp1 = line.split(None, 1)
#         sp2 = line.split()
#         if len(sp1) != 2: # validate input
#             continue
#         pid = int(sp2[0]) # first field is pid
#         cmd = sp2[1] # second field is command
#         if cmd == 'start':
#             port = int(sp2[3])
#             # sleep a while if a process is going to recover -- to avoid the
#             # case that the process is started but the previous one hasn't
#             # crashed.
#             if pid not in started_processes:
#                 started_processes[pid] = True
#             else:
#                 time.sleep(2)
#             # start the process
#             if debug:
#                 subprocess.Popen(['./process', str(pid), sp2[2], sp2[3]])
#             else:
#                 subprocess.Popen(['./process', str(pid), sp2[2], sp2[3]], stdout=open('/dev/null'), stderr=open('/dev/null'))
#             # sleep for a while to allow the process be ready
#             time.sleep(1)
#             # connect to the port of the pid
#             handler = ClientHandler(pid, address, port)
#             threads[pid] = handler
#             handler.start()
#         else:
#             if cmd == 'msg': # message msgid msg
#                 msgs[int(sp2[2])] = sp1[1]
#                 send(pid, sp1[1])
#             elif cmd[:5] == 'crash': # crashXXX
#                 send(pid, sp1[1])
#             elif cmd == 'get': # get chatLog
#                 if not wait_chat_log: # sleep for the first continous get command
#                     time.sleep(1)
#                 else:
#                     while wait_chat_log: # get command blocks next get command
#                         time.sleep(0.1)
#                 send(pid, sp1[1], set_wait=True)






def main():
    global threads, wait_chat_log, started_processes, debug
    timeout_thread = Thread(target=timeout, args=())
    timeout_thread.daemon = True
    timeout_thread.start()

    while True:
        line = ''
        try:
            line = sys.stdin.readline().strip()  # Read and strip line
        except:
            exit(True)  # Handle keyboard exception or EOF
        if line == 'exit':
            exit()

        # New command parsing logic for 'use <id> start <n>'
        if line.startswith('start'):
            _, pid_str, _, n_str = line.split()  # Extract pid and n from the command
            pid, n = int(pid_str), int(n_str)
            port = 20000 + pid  # Example port calculation, adjust as needed

            for i in range(n):  # Start 'n' servers
                current_pid = pid + i  # Calculate current server's ID
                # Ensure unique starting process logic here
                if current_pid not in started_processes:
                    started_processes[current_pid] = True
                else:
                    time.sleep(2)  # Sleep to allow process recovery

                # Start the server process
                if debug:
                    subprocess.Popen(['./server', str(current_pid), str(n), str(port + i)])
                else:
                    subprocess.Popen(['./server', str(current_pid), str(n), str(port + i)], stdout=open('/dev/null'),
                                     stderr=open('/dev/null'))

                # Connect to the server after it has started
                handler = ClientHandler(current_pid, address, port + i)
                threads[current_pid] = handler
                handler.start()

                time.sleep(1)  # Sleep to ensure server readiness
        else:
            sp1 = line.split(None, 1)
            sp2 = line.split()
            if len(sp1) != 2:  # validate input
                continue
            pid = int(sp2[0])  # first field is pid
            cmd = sp2[1]  # second field is command
            if cmd == 'msg':  # message msgid msg
                msgs[int(sp2[2])] = sp1[1]
                send(pid, sp1[1])
            elif cmd[:5] == 'crash':  # crashXXX
                send(pid, sp1[1])
            elif cmd == 'get':  # get chatLog
                if not wait_chat_log:  # sleep for the first continuous get command
                    time.sleep(1)
                else:
                    while wait_chat_log:  # get command blocks next get command
                        time.sleep(0.1)
                send(pid, sp1[1], set_wait=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'debug':
        debug = True
    main()
