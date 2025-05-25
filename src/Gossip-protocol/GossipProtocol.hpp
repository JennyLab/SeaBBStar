#ifndef GOSSIP_PROTOCAL_HPP
#define GOSSIP_PROTOCAL_HPP

#include "MerkleTree.hpp"
#include "NetworkCommunication.hpp"
#include "PeerSamplingService.hpp"
#include <chrono>
#include <future> // For std::async, std::future
#include <iostream>
#include <memory>
#include <mutex> // For std::mutex
#include <nlohmann/json.hpp>
#include <queue> // For std::queue
#include <string>
#include <thread>
#include <unordered_map>

class GossipProtocol
{
    std::unique_ptr<NetworkCommunication> netComm;
    std::shared_ptr<PeerSamplingService> peerSamplingService;
    std::unordered_map<std::string, std::string> state; // node status management
    std::mutex messageQueueMutex;
    std::queue<std::string> messageQueue; // Thread-safe queue for messages
    bool listening = true;                // Control flag for the listening thread
    std::thread listenerThread;
    MerkleTree stateTree; // A Merkle tree representing the node's current state
    std::mutex stateMutex;

    struct VersionedData
    {
        std::string value;
        uint64_t version;
    };

public:
    GossipProtocol(std::shared_ptr<PeerSamplingService> service)
        : peerSamplingService(std::move(service)),
          netComm(std::make_unique<NetworkCommunication>())
    {
        std::thread(&GossipProtocol::listenForMessages, this).detach(); // Start listening in a detached thread
    }

    ~GossipProtocol()
    {
        listening = false; // Stop the listening thread
    }

    void onJoin(const Node &newNode)
    {
        // Process a node joining
        std::cout << "Node joined: " << newNode.address << std::endl;
        // Send initial state to the new node
        sendState(newNode);
    }

    void onAlive(const Node &node)
    {
        std::cout << "Node alive: " << node.address << std::endl;
        refreshNodeStatus(node.address, "alive");
        broadcastStatusUpdate(node.address, "alive");
    }

    void onDead(const Node &node)
    {
        std::cout << "Node dead: " << node.address << std::endl;
        refreshNodeStatus(node.address, "dead");
        broadcastStatusUpdate(node.address, "dead");
    }

    void onChange(const std::string &key, const std::string &value)
    {
        // Handle state changes
        state[key] = value;
        std::cout << "State changed: " << key << " -> " << value << std::endl;
        // Gossip the change to a subset of peers
        gossipChange(key, value);
    }

    // Method to initiate state synchronization with another node
    void synchronizeStateWithPeer(const Node &peer)
    {
        // This could involve sending a checksum or Merkle tree root to the peer
        std::string myChecksum = calculateStateChecksum();
        std::string message = createSyncRequestMessage(myChecksum);
        netComm->send(message, peer.address, peer.port);
    }

    // Public methods as before, potentially including a way to trigger state synchronization manually
    void triggerStateSynchronization()
    {
        // Correct use of shared_ptr to access member function
        auto peers = peerSamplingService->getSamplePeers(1);
        for (const auto &peer : peers)
        {
            synchronizeStateWithPeer(peer);
        }
    }

private:
    void sendState(const Node &node)
    {
        // Serialize the state into a string format
        std::string serializedState = serializeState();
        // Use the netComm object to send the serialized state to the given node's address and port
        netComm->send(serializedState, node.address, node.port); // Use arrow operator
    }

    // Initialize Merkle Tree with the current state
    void initializeStateTree()
    {
        stateTree.initialize(state);
    }

    // Updates state and Merkle Tree
    void updateState(const std::string &key, const std::string &value)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        state[key] = value;
        stateTree.update(key, value); // Update the Merkle Tree with the change
    }

    void listenForMessages()
    {
        while (listening)
        {
            // for receiving messages - should be non-blocking or timeout-based
            std::string message; // Assume this gets populated by the network communication mechanism

            {
                std::lock_guard<std::mutex> lock(messageQueueMutex);
                messageQueue.push(message); // Safely push messages to the queue
            }
            // Concurrency: The listenForMessages method suggests setting up a listener for incoming gossip messages.
            // In practice, this would likely involve multithreading or asynchronous I/O to handle messages concurrently
            // without blocking the main program flow.
            // Process messages from the queue in another part of the application, or in a separate thread
        }
    }

    void broadcastStatusUpdate(const std::string &address, const std::string &status)
    {
        std::string message = createStatusMessage(address, status);
        int numberOfPeers = 5; // Example: broadcast to 5 peers
        auto peers = peerSamplingService->getSamplePeers(numberOfPeers);

        std::vector<std::future<void>> futures;

        for (const auto &peer : peers)
        {
            futures.emplace_back(
                std::async(std::launch::async,
                           [this, message, peer]()
                           {
                               try
                               {
                                   netComm->send(message, peer.address, peer.port); // Assuming netComm.send takes port as well
                               }
                               catch (const std::exception &e)
                               {
                                   std::cerr << "Failed to send message to " << peer.address << ": " << e.what() << std::endl;
                               }
                           }));
        }

        // wait for all tasks to complete, handling any exceptions they might throw
        for (auto &future : futures)
        {
            try
            {
                future.get(); // This will re-throw any exception caught in the async task
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error in sending message asynchronously: " << e.what() << std::endl;
            }
        }
    }

    // void broadcastStatusUpdate(const std::string &address, const std::string &status)
    // {
    //     std::string message = createStatusMessage(address, status);
    //     auto peers = peerSamplingService->getSamplePeers(5); // Correct access
    //     for (const auto &peer : peers)
    //     {
    //         std::async(std::launch::async, [this, message, &peer]() { // Correct capture
    //             netComm->send(message, peer.address, peer.port);      // Correct access
    //         });
    //     }
    // }

    // Method to process messages from the queue
    void processMessages()
    {
        while (true)
        { // Or some condition to stop processing
            std::string message;
            {
                std::lock_guard<std::mutex> lock(messageQueueMutex);
                if (!messageQueue.empty())
                {
                    message = messageQueue.front();
                    messageQueue.pop();
                }
            }

            if (!message.empty())
            {
                processReceivedMessage(message); // Process the message
            }

            // Include a mechanism to sleep or yield the thread to prevent busy waiting
        }
    }

    // std::string serializeState()
    // {
    //     // state serialization logic
    //     // let's concatenate key-value pairs separated by commas
    //     std::string serialized = "";
    //     for (const auto &pair : state)
    //     {
    //         serialized += pair.first + ":" + pair.second + ",";
    //     }
    //     // Remove the trailing comma
    //     if (!serialized.empty())
    //     {
    //         serialized.pop_back();
    //     }
    //     return serialized;
    // }

    std::string serializeState()
    {
        nlohmann::json jsonState = nlohmann::json::object();
        for (const auto &[key, value] : state)
        {
            jsonState[key] = value;
        }
        return jsonState.dump(); // Convert the state object to a string
    }

    void refreshNodeStatus(const std::string &address, const std::string &status)
    {
        // Check if the node exists in the status map and update its status
        auto it = state.find(address);
        if (it != state.end())
        {
            it->second = status;
        }
        else
        {
            // If the node is not in the map, add it with the given status
            state[address] = status;
        }

        // Depending on the protocol, you might want to broadcast this status update to other nodes
        broadcastStatusUpdate(address, status);
    }

    std::string createStatusMessage(const std::string &address, const std::string &status)
    {
        // Create a simple JSON-like format message or use a more sophisticated serialization method
        return "{\"node\":\"" + address + "\", \"status\":\"" + status + "\"}";
    }

    // Method to handle receiving status update messages - assume it's implemented elsewhere
    void processReceivedMessage(const std::string &message)
    {
        // Assuming the message is in a JSON format like:
        // {"node": "address", "status": "value"}
        try
        {
            auto j = nlohmann::json::parse(message);
            std::string address = j["node"];
            std::string status = j["status"];

            // Now, update the internal state with the received information
            {
                std::lock_guard<std::mutex> guard(messageQueueMutex); // Ensure thread safety when accessing 'state'
                state[address] = status;
            }

            std::cout << "Updated status for node " << address << " to " << status << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to process received message: " << e.what() << std::endl;
        }
    }

    void gossipChange(const std::string &key, const std::string &value)
    {
        // Select a peer node
        Node peerNode = peerSamplingService->getPeerNode();
        // Create a message containing the change
        std::string message = createMessage(key, value);
        // Send the message to the selected peer
        netComm->send(message, peerNode.address, peerNode.port);
    }

    std::string createMessage(const std::string &key, const std::string &value)
    {
        nlohmann::json message;
        message["key"] = key;
        message["value"] = value;
        return message.dump(); // Serialize the JSON object to a string
    }

    void broadcastMessage(const std::string &message)
    {
        auto peers = peerSamplingService->getSamplePeers(5); // Get 5 random peers
        std::vector<std::future<void>> futures;

        for (const auto &peer : peers)
        {
            auto future = std::async(std::launch::async, [this, message, &peer]()
                                     { netComm->send(message, peer.address, peer.port); });
            futures.push_back(std::move(future));
        }

        // Wait for all futures to complete
        for (auto &future : futures)
        {
            future.get(); // This will block until the task is complete
        }
    }

    void updateNodeStatus(const std::string &nodeId, const std::string &status)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            state[nodeId] = status;
            std::cout << "Updated status for node " << nodeId << " to " << status << std::endl;
        }

        // Prepare the message for broadcasting
        nlohmann::json message;
        message["type"] = "statusUpdate";
        message["data"] = {{"nodeId", nodeId}, {"status", status}};
        broadcastMessage(message.dump());
    }

    void handleJoinRequest(const std::string &nodeId, const std::string &address, int port)
    {
        std::cout << "Node " << nodeId << " with address " << address << ":" << port << " requested to join." << std::endl;
        // Assuming addNode now accepts a Node object correctly constructed
        Node newNode(address, port);           // Correct after constructor update
        peerSamplingService->addNode(newNode); // Correct access
        // Send the current network state to the new node
        std::string serializedState = serializeState();
        netComm->send(serializedState, address, port); // Correct access
    }

    // Method to process received messages
    void processMessage(const std::string &message)
    {
        try
        {
            auto parsedMessage = nlohmann::json::parse(message);
            std::string messageType = parsedMessage["type"];
            auto data = parsedMessage["data"];

            if (messageType == "statusUpdate")
            {
                std::string nodeId = data["nodeId"];
                std::string status = data["status"];
                updateNodeStatus(nodeId, status);
            }
            else if (messageType == "joinRequest")
            {
                std::string nodeId = data["nodeId"];
                std::string nodeAddress = data["address"];
                int nodePort = data["port"];
                handleJoinRequest(nodeId, nodeAddress, nodePort);
            }
            else
            {
                std::cerr << "Unknown message type received: " << messageType << std::endl;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to process received message: " << e.what() << std::endl;
        }
    }

    // Utility method to create a synchronization request message
    std::string createSyncRequestMessage(const std::string &checksum)
    {
        nlohmann::json requestMessage;
        requestMessage["type"] = "syncRequest";
        requestMessage["data"] = {
            {"checksum", checksum}};
        return requestMessage.dump(); // Convert the JSON object to a string
    }

    nlohmann::json serializeDifferences(const std::vector<std::string> &differences)
    {
        // Convert differences into a JSON object or array
        nlohmann::json serialized;
        for (const auto &diff : differences)
        {
            // Serialization logic here, depending on how differences are structured
        }
        return serialized;
    }

    // Method to calculate the root hash of the current state
    std::string calculateStateChecksum()
    {
        return stateTree.getRootHash();
    }

    // Method to identify differences and generate a list of changes
    std::vector<std::string> identifyDifferences(const std::string &receivedChecksum)
    {
        if (receivedChecksum != stateTree.getRootHash())
        {
            return stateTree.getDifferingLeaves(receivedChecksum);
        }
        return {};
    }

    // Method to handle a state synchronization request
    void handleSyncRequest(const std::string &receivedChecksum, const Node &requester)
    {
        std::string myChecksum = calculateStateChecksum();

        // Check if the checksums (or Merkle tree roots) are different
        if (myChecksum != receivedChecksum)
        {
            // Use Merkle trees or another method to identify specific differences
            auto differences = identifyDifferences(receivedChecksum);

            if (!differences.empty())
            {
                // Prepare and send a message with just the differences
                nlohmann::json updateMessage;
                updateMessage["type"] = "stateUpdate";
                updateMessage["data"] = serializeDifferences(differences);

                netComm->send(updateMessage.dump(), requester.address, requester.port);
            }
        }
        else
        {
            std::cout << "No differences detected, states are synchronized." << std::endl;
        }
    }
};

#endif
