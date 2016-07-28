#include "Networking.hpp"

std::mutex coutMutex;

std::string serializeMapSize(const hlt::Map & map) {
	std::string returnString = "";
	std::ostringstream oss;
	oss << map.map_width << ' ' << map.map_height << ' ';
	returnString = oss.str();
	return returnString;
}

std::string serializeProductions(const hlt::Map & map) {
	std::string returnString = "";
	std::ostringstream oss;
	for(auto a = map.contents.begin(); a != map.contents.end(); a++) {
		for(auto b = a->begin(); b != a->end(); b++) {
			oss << (unsigned short)(b->production) << ' ';
		}
	}
	returnString = oss.str();
	return returnString;
}

std::string Networking::serializeMap(const hlt::Map & map) {
	std::string returnString = "";
	std::ostringstream oss;

	//Run-length encode of owners
	unsigned short currentOwner = map.contents[0][0].owner;
	unsigned short counter = 0;
	for(int a = 0; a < map.contents.size(); ++a) {
		for(int b = 0; b < map.contents[a].size(); ++b) {
			if(map.contents[a][b].owner == currentOwner) {
				counter++;
			}
			else {
				oss << (unsigned short)counter << ' ' << (unsigned short)currentOwner << ' ';
				counter = 1;
				currentOwner = map.contents[a][b].owner;
			}
		}
	}
	//Place the last run into the string
	oss << (unsigned short)counter << ' ' << (unsigned short)currentOwner << ' ';

	//Encoding of ages
	for(int a = 0; a < map.contents.size(); ++a) {
		for(int b = 0; b < map.contents[a].size(); ++b) {
			oss << (unsigned short)map.contents[a][b].strength << ' ';
		}
	}

	returnString = oss.str();

	return returnString;
}

std::set<hlt::Move> Networking::deserializeMoveSet(std::string & inputString, const hlt::Map & m) {
	std::set<hlt::Move> moves = std::set<hlt::Move>();

	std::stringstream iss(inputString);
	hlt::Location l;
	int d;
	while (iss >> l.x >> l.y >> d && m.inBounds(l)) moves.insert({ l, (unsigned char)d });

	return moves;
}

void Networking::sendString(unsigned char playerTag, std::string &sendString) {
	//End message with newline character
	sendString += '\n';

#ifdef _WIN32
	WinConnection connection = connections[playerTag - 1];

	DWORD charsWritten;
	bool success;
	success = WriteFile(connection.write, sendString.c_str(), sendString.length(), &charsWritten, NULL);
	if(!success || charsWritten == 0) {
		if(!quiet_output) std::cout << "Problem writing to pipe\n";
		throw 1;
	}
#else
	UniConnection connection = connections[playerTag - 1];
	ssize_t charsWritten = write(connection.write, sendString.c_str(), sendString.length());
	if(charsWritten < sendString.length()) {
		if(!quiet_output) std::cout << "Problem writing to pipe\n";
		throw 1;
	}
#endif
}

std::string Networking::getString(unsigned char playerTag, unsigned int timeoutMillis) {

	std::string newString;
#ifdef _WIN32
	WinConnection connection = connections[playerTag - 1];

	DWORD charsRead;
	bool success;
	char buffer;

	//Keep reading char by char until a newline
	while (true) {
		//Check to see that there are bytes in the pipe before reading
		//Throw error if no bytes in alloted time
		//Check for bytes before sampling clock, because reduces latency (vast majority the pipe is alread full)
		DWORD bytesAvailable = 0;
		PeekNamedPipe(connection.read, NULL, 0, NULL, &bytesAvailable, NULL);
		if(bytesAvailable < 1) {
			std::chrono::high_resolution_clock::time_point initialTime = std::chrono::high_resolution_clock::now();
			while (bytesAvailable < 1) {
				if(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - initialTime).count() > timeoutMillis) throw 1;
				PeekNamedPipe(connection.read, NULL, 0, NULL, &bytesAvailable, NULL);
			}
		}

		success = ReadFile(connection.read, &buffer, 1, &charsRead, NULL);
		if(!success || charsRead < 1) {
			if(!quiet_output) std::cout << "Pipe probably timed out\n";
			throw 1;
		}
		if(buffer == '\n') break;
		else newString += buffer;
	}
#else
	UniConnection connection = connections[playerTag - 1];

	fd_set set;
	FD_ZERO(&set); /* clear the set */
	FD_SET(connection.read, &set); /* add our file descriptor to the set */

	struct timeval timeout; //Non-blocking. We'll check every ten millisecond to get a result.
	timeout.tv_sec = 0;
	timeout.tv_usec = 10000;

	char buffer;
	
	//The time we started at.
	std::chrono::high_resolution_clock::time_point initialTime = std::chrono::high_resolution_clock::now();

	//Keep reading char by char until a newline
	bool shouldContinue = true;
	while(shouldContinue) {
		//Check if process is dead.
		int status;
		if(waitpid(processes[playerTag - 1], &status, WNOHANG) == processes[playerTag - 1] || std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - initialTime).count() >= timeoutMillis) {
			killPlayer(playerTag);
			std::cout << "Error!" << std::endl;
			if(!quiet_output) {
				// Buffer error message output
				// If a bunch of bots fail at onces, we dont want to be writing to cout at the same time
				// That looks really weird
				std::string errorMessage = "";
				errorMessage += std::string("Unix bot timed out or errored.\n");

				playerLogs[playerTag-1].push_back(newString);
				errorMessage += "#---------ALL OF THE OUTPUT OF THE BOT THAT TIMED OUT----------#\n";
				for(auto stringIter = playerLogs[playerTag-1].begin(); stringIter != playerLogs[playerTag-1].end(); stringIter++) {
					while(stringIter->size() < 60) stringIter->push_back(' ');
					errorMessage += "# " + *stringIter + " #\n";
				}
				errorMessage += "#--------------------------------------------------------------#\n";

				std::lock_guard<std::mutex> guard(coutMutex);
				std::cout << errorMessage;
			}
			throw 1;
		}

		//Check if there are bytes in the pipe
		for(int selectionResult = select(connection.read+1, &set, NULL, NULL, &timeout); selectionResult > 0; selectionResult--) {
			read(connection.read, &buffer, 1);

			if(buffer == '\n') {
				shouldContinue = false;
				break;
			}
			else newString += buffer;
		}

		//Reset timeout - we should consider it to be undefined.
		timeout.tv_sec = 0;
		timeout.tv_usec = 10000;
	}
#endif
	//Python turns \n into \r\n
	if(newString.at(newString.size() - 1) == '\r') newString.pop_back();

	playerLogs[playerTag-1].push_back(newString);

	return newString;
}

void Networking::startAndConnectBot(std::string command) {
#ifdef _WIN32
	command = "/C " + command;

	WinConnection parentConnection, childConnection;

	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	//Child stdout pipe
	if(!CreatePipe(&parentConnection.read, &childConnection.write, &saAttr, 0)) {
		if(!quiet_output) std::cout << "Could not create pipe\n";
		throw 1;
	}
	if(!SetHandleInformation(parentConnection.read, HANDLE_FLAG_INHERIT, 0)) throw 1;

	//Child stdin pipe
	if(!CreatePipe(&childConnection.read, &parentConnection.write, &saAttr, 0)) {
		if(!quiet_output) std::cout << "Could not create pipe\n";
		throw 1;
	}
	if(!SetHandleInformation(parentConnection.write, HANDLE_FLAG_INHERIT, 0)) throw 1;

	//MAKE SURE THIS MEMORY IS ERASED
	PROCESS_INFORMATION piProcInfo;
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

	STARTUPINFO siStartInfo;
	ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
	siStartInfo.cb = sizeof(STARTUPINFO);
	siStartInfo.hStdError = childConnection.write;
	siStartInfo.hStdOutput = childConnection.write;
	siStartInfo.hStdInput = childConnection.read;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	//C:/xampp/htdocs/Halite/Halite/Debug/ExampleBot.exe
	//C:/Users/Michael/Anaconda3/python.exe
	//C:/Program Files/Java/jre7/bin/java.exe -cp C:/xampp/htdocs/Halite/AIResources/Java MyBot
	bool success = CreateProcess(
		"C:\\windows\\system32\\cmd.exe",
		LPSTR(command.c_str()),     //command line
		NULL,          //process security attributes
		NULL,          //primary thread security attributes
		TRUE,          //handles are inherited
		0,             //creation flags
		NULL,          //use parent's environment
		NULL,          //use parent's current directory
		&siStartInfo,  //STARTUPINFO pointer
		&piProcInfo
	);  //receives PROCESS_INFORMATION
	if(!success) {
		if(!quiet_output) std::cout << "Could not start process\n";
		throw 1;
	}
	else {
		CloseHandle(piProcInfo.hProcess);
		CloseHandle(piProcInfo.hThread);

		processes.push_back(piProcInfo.hProcess);
		connections.push_back(parentConnection);
	}
#else
	if(!quiet_output) std::cout << command << "\n";

	pid_t pid = (pid_t)NULL;
	int writePipe[2];
	int readPipe[2];

	if(pipe(writePipe)) {
		if(!quiet_output) std::cout << "Error creating pipe\n";
		throw 1;
	}
	if(pipe(readPipe)) {
		if(!quiet_output) std::cout << "Error creating pipe\n";
		throw 1;
	}

	//Fork a child process
	pid = fork();
	if(pid == 0) { //This is the child
		setpgid(getpid(), getpid());

		dup2(writePipe[0], STDIN_FILENO);

		dup2(readPipe[1], STDOUT_FILENO);
		dup2(readPipe[1], STDERR_FILENO);

		execl("/bin/sh", "sh", "-c", command.c_str(), (char*) NULL);

		//Nothing past the execl should be run

		exit(1);
	} else if(pid < 0) {
		if(!quiet_output) std::cout << "Fork failed\n";
		throw 1;
	}

	UniConnection connection;
	connection.read = readPipe[0];
	connection.write = writePipe[1];

	connections.push_back(connection);
	processes.push_back(pid);

#endif

	playerLogs.push_back(std::vector<std::string>(0));
}

void Networking::handleInitNetworking(unsigned char playerTag, const hlt::Map & m, int * playermillis, std::string * playerName) {
	try{
		std::string playerTagString = std::to_string(playerTag), mapSizeString = serializeMapSize(m), mapString = serializeMap(m), prodString = serializeProductions(m);
		sendString(playerTag, playerTagString);
		sendString(playerTag, mapSizeString);
		sendString(playerTag, prodString);
		sendString(playerTag, mapString);
		std::string outMessage = "Init Message sent to player " + std::to_string(int(playerTag)) + ".\n";
		if(!quiet_output) std::cout << outMessage;

		std::chrono::high_resolution_clock::time_point initialTime = std::chrono::high_resolution_clock::now();
		*playerName = getString(playerTag, *playermillis).substr(0, 30);
		unsigned int millisTaken = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - initialTime).count();
		std::string inMessage = "Init Message received from player " + std::to_string(int(playerTag)) + ", " + *playerName + ".\n";
		if(!quiet_output) std::cout << inMessage;

		*playermillis -= millisTaken;
	}
	catch(...) {
		*playerName = "Bot #" + std::to_string(playerTag) + "; timed out during Init";
		*playermillis = -1;
	}
}

void Networking::handleFrameNetworking(unsigned char playerTag, const hlt::Map & m, int * playermillis, std::set<hlt::Move> * moves) {
	try{
		if(isProcessDead(playerTag)) return;

		//Send this bot the game map and the messages addressed to this bot
		std::string mapString = serializeMap(m);
		sendString(playerTag, mapString);

		moves->clear();

		std::chrono::high_resolution_clock::time_point initialTime = std::chrono::high_resolution_clock::now();
		std::string movesString = getString(playerTag, *playermillis);
		unsigned int millisTaken = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - initialTime).count();

		*moves = deserializeMoveSet(movesString, m);
		
		*playermillis -= millisTaken;
	}
	catch(...) {
		*moves = std::set<hlt::Move>();
		*playermillis = -1;
	}

}

void Networking::killPlayer(unsigned char playerTag) {
	if(isProcessDead(playerTag)) return;
#ifdef _WIN32

	HANDLE process = processes[playerTag - 1];

	TerminateProcess(process, 0);

	processes[playerTag - 1] = NULL;
	connections[playerTag - 1].read = NULL;
	connections[playerTag - 1].write = NULL;

	if(!quiet_output) std::cout << "Player " << int(playerTag) << " is dead\n";
#else
	kill(-processes[playerTag - 1], SIGKILL);

	processes[playerTag - 1] = -1;
	connections[playerTag - 1].read = -1;
	connections[playerTag - 1].write = -1;
#endif
}

bool Networking::isProcessDead(unsigned char playerTag) {
#ifdef _WIN32
	return processes[playerTag - 1] == NULL;
#else
	return processes[playerTag - 1] == -1;
#endif
}

int Networking::numberOfPlayers() {
#ifdef _WIN32
	return connections.size();
#else
	return connections.size();
#endif
}
