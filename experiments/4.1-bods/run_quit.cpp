#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <vector>

#include "quick-insertion-tree/src/bptree/memory_block_manager.h"
// bp_node.h must NOT be included directly; bp_tree.h includes it after
// setting up the BlockManager typedef.
#include "quick-insertion-tree/src/bptree/bp_tree.h"

using namespace std;

using key_type = uint32_t;
using value_type = uint32_t;

vector<key_type> read_bin(const char* filename) {
    ifstream f(filename, ios::binary | ios::ate);
    streamsize size = f.tellg();
    f.seekg(0, ios::beg);
    vector<key_type> data(size / sizeof(key_type));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

vector<key_type> read_txt(const char* filename) {
    ifstream f(filename);
    vector<key_type> data;
    key_type val;
    while (f >> val) data.push_back(val);
    return data;
}

vector<key_type> read_file(const string& filename) {
    if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".txt")
        return read_txt(filename.c_str());
    return read_bin(filename.c_str());
}

int main(int argc, char** argv) {
    bool verbose = false;
    int N = 500000000;
    string input_file = "";
#ifdef QUIT_FAT
    string tree_type = "QuIT";
#else
    string tree_type = "BPTree";
#endif

    for (int i = 1; i < argc;) {
        if (string(argv[i]) == "-v") {
            verbose = true;
            i++;
        } else if (string(argv[i]) == "-N") {
            N = atoi(argv[i + 1]);
            i += 2;
        } else if (string(argv[i]) == "-f") {
            input_file = argv[i + 1];
            i += 2;
        } else if (string(argv[i]) == "-t") {
            tree_type = argv[i + 1];
            i += 2;
        } else {
            i++;
        }
    }

    if (input_file.empty()) {
        cerr << "Usage: " << argv[0] << " -f <input_file> [-N <count>] [-t BPTree|QuIT] [-v]\n";
        return 1;
    }

    if (tree_type != "BPTree" && tree_type != "QuIT") {
        cerr << "Unknown tree type: " << tree_type << ". Use BPTree or QuIT.\n";
        return 1;
    }

    auto keys = read_file(input_file);
    if ((size_t)N > keys.size()) N = (int)keys.size();

    // Worst-case leaf count is N / SPLIT_LEAF_POS = N / 128 (half-full splits).
    // Add a small buffer for internal nodes.
    size_t blocks_needed = (size_t)N / 100 + 10000;
    InMemoryBlockManager manager("", (uint32_t)blocks_needed);
    bp_tree<key_type, value_type> tree(manager);

    long long insertion_time = 0;
    if (tree_type == "BPTree") {
        for (int i = 0; i < N; i++) {
            auto start = chrono::high_resolution_clock::now();
            tree.top_insert(keys[i], keys[i]);
            auto stop = chrono::high_resolution_clock::now();
            insertion_time += chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
        }
    } else {
        for (int i = 0; i < N; i++) {
            auto start = chrono::high_resolution_clock::now();
            tree.insert(keys[i], keys[i]);
            auto stop = chrono::high_resolution_clock::now();
            insertion_time += chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
        }
    }

    srand(time(0));
    int Q = N / 100;
    long long query_time = 0;
    for (int i = 0; i < Q; i++) {
        int idx = rand() % N;
        auto start = chrono::high_resolution_clock::now();
        bool found = tree.contains(keys[idx]);
        auto stop = chrono::high_resolution_clock::now();
        query_time += chrono::duration_cast<chrono::nanoseconds>(stop - start).count();
        (void)found;
    }

    if (verbose) {
        cerr << "Tree type: " << tree_type << "\n";
        cerr << "Insertion time: " << insertion_time << " ns\n";
        cerr << "Query time: " << query_time << " ns\n";
    }

    cout << insertion_time << "," << query_time << endl;
    return 0;
}
