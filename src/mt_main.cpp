#include "Gossip-protocol/MerkleTree.hpp"
#include <unordered_map>

int main()
{
    // Example state data
    std::unordered_map<std::string, std::string> state{
        {"key1", "value1"},
        {"key2", "value2"},
        // Add more key-value pairs as needed
    };

    // Initialize the Merkle Tree with the state
    MerkleTree tree;
    tree.initialize(state);

    // Retrieve and display the root hash
    std::cout << "Root Hash: " << tree.getRootHash() << std::endl;

    // Remember, update and getDifferingLeaves functionalities are placeholders and would need proper implementation for full functionality.
    return 0;
}

// g++ -std=c++17 -o mt_main mt_main.cpp
