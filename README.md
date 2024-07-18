Custom HTTP Server in C
This is a custom HTTP server implemented in C, capable of handling GET and POST requests. It supports serving files and returning header information for GET requests, as well as accepting and processing data for POST requests.

Features
GET Requests: Retrieve header information and files from the server.
POST Requests: Accept data posted to the server.
Asynchronous Handling: Uses epoll for asynchronous I/O operations.
Limited Concurrency: Supports up to 10 simultaneous clients.
Getting Started
Prerequisites
Linux-based operating system
GCC compiler
Basic knowledge of using terminal and make commands
Building and Running the Server
Clone the repository:

bash
Copy code
git clone https://github.com/kbanushi/Custom-HTTP-Server.git
cd your-repo
Build the server:

bash
Copy code
make
Run the server:

bash
Copy code
Replace <port> with the port number you want the server to listen on (e.g., 8080) in the port.txt file

Testing the Application
The repository includes a test script (runtests.sh) and a folder (tests/) containing test cases.

Ensure the server is running on a separate terminal or in the background.

Run the tests:

bash
Copy code
./runtests.sh
This script will execute various tests located in the tests/ folder to verify different functionalities of the server.

Contributing
Contributions are welcome! If you'd like to contribute to this project, please fork the repository and submit a pull request. Feel free to open issues for feature requests, bug reports, or general feedback.
