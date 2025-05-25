#ifndef NETWORK_COMMUNICATION_HPP
#define NETWORK_COMMUNICATION_HPP

#include <cstring>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using json = nlohmann::json;

struct Message
{
    std::string type;
    std::string origin;
    int seqNo;
    std::string text;
    std::map<std::string, int> status;

    // Convert Message to JSON
    json toJson() const
    {
        json j;
        j["type"] = type;
        j["origin"] = origin;
        j["seqNo"] = seqNo;
        j["text"] = text;
        j["status"] = status;
        return j;
    }

    // Initialize Message from JSON
    static Message fromJson(const json &j)
    {
        Message msg;
        msg.type = j.at("type").get<std::string>();
        msg.origin = j.at("origin").get<std::string>();
        msg.seqNo = j.at("seqNo").get<int>();
        msg.text = j.at("text").get<std::string>();
        msg.status = j.at("status").get<std::map<std::string, int>>();
        return msg;
    }
};

class NetworkCommunication
{
public:
    void send(const Message &message, const std::string &hostname, int port)
    {
        int sockfd;
        struct sockaddr_in serv_addr;
        struct hostent *server;

        // Serialize message to JSON
        std::string messageJson = message.toJson().dump();

        // Create socket
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            std::cerr << "ERROR opening socket" << std::endl;
            return;
        }

        server = gethostbyname(hostname.c_str());
        if (server == NULL)
        {
            std::cerr << "ERROR, no such host" << std::endl;
            close(sockfd);
            return;
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
        serv_addr.sin_port = htons(port);

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            std::cerr << "ERROR connecting" << std::endl;
            close(sockfd);
            return;
        }

        // Send the message
        int n = write(sockfd, messageJson.c_str(), messageJson.length());
        if (n < 0)
        {
            std::cerr << "ERROR writing to socket" << std::endl;
        }

        close(sockfd);
    }

    Message receive(int port)
    {
        int sockfd, newsockfd;
        socklen_t clilen;
        char buffer[1024]; // Adjust buffer size if necessary
        struct sockaddr_in serv_addr, cli_addr;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            std::cerr << "ERROR opening socket" << std::endl;
            return Message(); // Return an empty message or handle differently
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(port);

        if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            std::cerr << "ERROR on binding" << std::endl;
            close(sockfd);
            return Message(); // Return an empty message or handle differently
        }

        listen(sockfd, 5);
        clilen = sizeof(cli_addr);

        newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (newsockfd < 0)
        {
            std::cerr << "ERROR on accept" << std::endl;
            close(sockfd);
            return Message(); // Return an empty message or handle differently
        }

        memset(buffer, 0, 1024);
        int n = read(newsockfd, buffer, 1023);
        if (n < 0)
        {
            std::cerr << "ERROR reading from socket" << std::endl;
            close(newsockfd);
            close(sockfd);
            return Message(); // Return an empty message or handle differently
        }

        close(newsockfd);
        close(sockfd);

        // Deserialize message from JSON
        json messageJson = json::parse(std::string(buffer));
        return Message::fromJson(messageJson);
    }
};

#endif
