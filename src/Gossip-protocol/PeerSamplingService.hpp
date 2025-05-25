#ifndef PEER_SAMPLING_SERVICE_HPP
#define PEER_SAMPLING_SERVICE_HPP

#include <algorithm> // For std::remove_if
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

class Node
{
public:
    std::string address;
    int port;

    Node() : port(0) {}                                                              // Default constructor
    Node(std::string address, int port) : address(std::move(address)), port(port) {} // Existing constructor
};

class PeerSamplingService
{
    std::vector<Node> nodes;
    mutable std::mutex nodesMutex; // mutable allows const methods to lock the mutex

public:
    PeerSamplingService(const std::vector<Node> &initialNodes) : nodes(initialNodes) {}

    std::vector<Node> getInitialNodes()
    {
        std::lock_guard<std::mutex> lock(nodesMutex);
        return nodes;
    }

    void addNode(const Node &node)
    {
        std::lock_guard<std::mutex> lock(nodesMutex);
        nodes.push_back(node);
    }

    void removeNode(const std::string &address)
    {
        std::lock_guard<std::mutex> lock(nodesMutex);
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                   [&address](const Node &node)
                                   { return node.address == address; }),
                    nodes.end());
    }

    Node getPeerNode()
    {
        std::lock_guard<std::mutex> lock(nodesMutex);
        if (nodes.size() < 1)
        {
            throw std::runtime_error("No available peers.");
        }

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, nodes.size() - 1);
        return nodes[dis(gen)];
    }

    // Returns a random sample of peers from the list of nodes
    std::vector<Node> getSamplePeers(size_t numberOfPeers)
    {
        std::lock_guard<std::mutex> lock(nodesMutex);
        std::vector<Node> shuffledNodes = nodes; // Copy the original list

        // Using a random shuffle to randomize the nodes list
        std::shuffle(shuffledNodes.begin(), shuffledNodes.end(), std::default_random_engine(std::random_device{}()));

        if (shuffledNodes.size() > numberOfPeers)
        {
            shuffledNodes.resize(numberOfPeers); // Keep only the desired number of peers
        }

        return shuffledNodes;
    }
};

#endif // PEER_SAMPLING_SERVICE_HPP
