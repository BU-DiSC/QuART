#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

#include "ART.h"
#include "ArtNode.h"
#include "Helper.h"
#include "trees/QuART_lil.h"
#include "trees/QuART_tail.h"
#include "trees/QuART_stail.h"
#include "QuArtNodeBulkLoadMethods.cpp"

using namespace std;

template <typename key_type>
std::vector<key_type> read_bin(const char* filename) {
    std::ifstream inputFile(filename, std::ios::binary | std::ios::ate);
    std::streamsize size = inputFile.tellg();
    inputFile.seekg(0, std::ios::beg);
    std::vector<key_type> data(size / sizeof(key_type));
    inputFile.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

template <typename key_type>
std::vector<key_type> read_txt(const char* filename) {
    std::ifstream inputFile(filename);
    std::vector<key_type> data;
    key_type val;
    while (inputFile >> val) {
        data.push_back(val);
    }
    return data;
}

template <typename key_type>
std::vector<key_type> read_file(const std::string& filename) {
    if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".txt") {
        return read_txt<key_type>(filename.c_str());
    }
    return read_bin<key_type>(filename.c_str());
}

int main(int argc, char** argv) {
    bool verbose = false;      // optional argument
    int N = 500000000;         // optional argument
    string input_file = "";                // optional argument (-f)
    string tree_type = "ART";  // default tree type
    bool use_bulkload = false; // optional argument
    bool use_synthetic = false; // optional argument: generate keys 1..N instead of reading a file
    
    // Query 1% of entries
    uint64_t minval = 0;

    // Parse arguments; make sure to increment i by 2 if you consume an argument
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
        } else if (string(argv[i]) == "--bulkload") {
            use_bulkload = true;
            i++;
        } else if (string(argv[i]) == "--synthetic") {
            use_synthetic = true;
            i++;
        } else {
            i++;
        }
    }

    uint64_t maxval = N-1;

    // read data
    std::vector<ART::key_int_t> keys;
    if (use_synthetic) {
        keys.resize(N);
        for (int i = 0; i < N; i++) keys[i] = static_cast<ART::key_int_t>(i + 1);
    } else {
        keys = read_file<ART::key_int_t>(input_file);
    }

    if (tree_type == "ART") {
        ART::ART* tree = new ART::ART();
        long long insertion_time = 0;
        if (use_bulkload) {
            // Create keys vector with 0 at the beginning, followed by N keys from file
            std::vector<ART::key_int_t> keys_to_load;
            keys_to_load.reserve(N + 1);
            keys_to_load.push_back(0);
            for (ART::key_int_t i = 1; i <= static_cast<ART::key_int_t>(N); i++) {
                keys_to_load.push_back(i);
            }
            auto start = chrono::high_resolution_clock::now();
            tree->bulkLoad(keys_to_load, keys_to_load);
            int compressed = tree->compressTree();
            auto stop = chrono::high_resolution_clock::now();
            insertion_time = chrono::duration_cast<chrono::nanoseconds>(stop - start).count();

            srand(time(0));
            long long query_time = 0;
            for (uint64_t i = 0; i < (uint64_t)N / 100; i++) {
                int random = rand() % N + 1;
                uint8_t key[ART::keyBytes];
                ART::loadKey(keys_to_load[random], key);
                auto start = chrono::high_resolution_clock::now();
                ART::ArtNode* leaf = tree->lookup(key);
                auto stop = chrono::high_resolution_clock::now();
                auto duration = chrono::duration_cast<chrono::nanoseconds>(stop - start);
                query_time += duration.count();
                assert(ART::isLeaf(leaf) && ART::getLeafValue(leaf) == keys_to_load[random]);
            }

            if (verbose) {
                cout << "Tree type: " << tree_type << endl;
                cout << "Insertion time: " << insertion_time << " ns" << endl;
                cout << "Query time: " << query_time << " ns" << endl;
            }

            cout << insertion_time << "," << query_time << endl;
            return 0;
        } else {
            // Regular ART insertion
            for (uint64_t i = 0; i < N; i++) {
                uint8_t key[ART::keyBytes];
                ART::loadKey(keys[i], key);
                auto start = chrono::high_resolution_clock::now();
                tree->insert(key, keys[i]);
                auto stop = chrono::high_resolution_clock::now();
                auto duration =
                    chrono::duration_cast<chrono::nanoseconds>(stop - start);
                insertion_time += duration.count();
            }

            srand(time(0));
            long long query_time = 0;
            for (uint64_t i = 0; i < (uint64_t)N / 100; i++) {
                int random = rand() % (maxval - minval + 1) + minval;
                uint8_t key[ART::keyBytes];
                ART::loadKey(keys[random], key);
                auto start = chrono::high_resolution_clock::now();
                ART::ArtNode* leaf = tree->lookup(key);
                auto stop = chrono::high_resolution_clock::now();
                auto duration = chrono::duration_cast<chrono::nanoseconds>(stop - start);
                query_time += duration.count();
                assert(ART::isLeaf(leaf) && ART::getLeafValue(leaf) == keys[random]);
            }

            if (verbose) {
                cout << "Tree type: " << tree_type << endl;
                cout << "Insertion time: " << insertion_time << " ns" << endl;
                cout << "Query time: " << query_time << " ns" << endl;
            }

            cout << insertion_time << "," << query_time << endl;
        }
    } else if (tree_type == "QuART_tail") {
        ART::QuART_tail* tree = new ART::QuART_tail();
        long long insertion_time = 0;
        for (uint64_t i = 0; i < N; i++) {
            uint8_t key[ART::keyBytes];
            ART::loadKey(keys[i], key);
            auto start = chrono::high_resolution_clock::now();
            tree->insert(key, keys[i]);
            //cout << "Inserted key: " << keys[i] << endl;
            //cout  << "Current fp_leaf value: " << (tree->fp_leaf ? ART::getLeafValue(tree->fp_leaf) : -1) << endl;
            auto stop = chrono::high_resolution_clock::now();
            auto duration =
                chrono::duration_cast<chrono::nanoseconds>(stop - start);
            insertion_time += duration.count();
        }

        if (verbose) {
            cout << "Tree type: " << tree_type << endl;
            cout << "Insertion time: " << insertion_time << " ns" << endl;
        }

        srand(time(0));

        long long query_time = 0;
        for (uint64_t i = 0; i < (uint64_t)N / 100; i++) {
            int random = rand() % (maxval - minval + 1) + minval;
            uint8_t key[ART::keyBytes];
            ART::loadKey(keys[random], key);
            auto start = chrono::high_resolution_clock::now();
            ART::ArtNode* leaf = tree->lookup(key);
            auto stop = chrono::high_resolution_clock::now();
            auto duration =
                chrono::duration_cast<chrono::nanoseconds>(stop - start);
            query_time += duration.count();
            if (!ART::isLeaf(leaf) || ART::getLeafValue(leaf) != keys[random]) {
                cerr << "Query failed: index=" << random << " key=" << keys[random] << " got=" << (ART::isLeaf(leaf) ? (int)ART::getLeafValue(leaf) : -1) << endl;
                assert(false);
            }
        }

        if (verbose) {
            cout << "Query time: " << query_time << " ns" << endl;
        }

        // Output the times in csv format, including tree type
        cout << insertion_time << "," << query_time << endl;
    } else if (tree_type == "QuART_lil") {
        ART::QuART_lil* tree = new ART::QuART_lil();
        long long insertion_time = 0;
        for (uint64_t i = 0; i < N; i++) {
            uint8_t key[ART::keyBytes];
            ART::loadKey(keys[i], key);
            auto start = chrono::high_resolution_clock::now();
            tree->insert(key, keys[i]);
            auto stop = chrono::high_resolution_clock::now();
            auto duration =
                chrono::duration_cast<chrono::nanoseconds>(stop - start);
            insertion_time += duration.count();
        }

        if (verbose) {
            cout << "Tree type: " << tree_type << endl;
            cout << "Insertion time: " << insertion_time << " ns" << endl;
        }

        srand(time(0));

        long long query_time = 0;
        for (uint64_t i = 0; i < (uint64_t)N / 100; i++) {
            int random = rand() % (maxval - minval + 1) + minval;
            uint8_t key[ART::keyBytes];
            ART::loadKey(keys[random], key);
            auto start = chrono::high_resolution_clock::now();
            ART::ArtNode* leaf = tree->lookup(key);
            auto stop = chrono::high_resolution_clock::now();
            auto duration =
                chrono::duration_cast<chrono::nanoseconds>(stop - start);
            query_time += duration.count();
            assert(ART::isLeaf(leaf) &&
                   ART::getLeafValue(leaf) == keys[random]);
        }

        if (verbose) {
            cout << "Query time: " << query_time << " ns" << endl;
        }

        // Output the times in csv format, including tree type
        cout << insertion_time << "," << query_time << endl;
    } else if (tree_type == "QuART_stail") {
        ART::QuART_stail* tree = new ART::QuART_stail();
        long long insertion_time = 0;
        for (uint64_t i = 0; i < N; i++) {
            uint8_t key[ART::keyBytes];
            ART::loadKey(keys[i], key);
            auto start = chrono::high_resolution_clock::now();
            tree->insert(key, keys[i]);
            auto stop = chrono::high_resolution_clock::now();
            auto duration =
                chrono::duration_cast<chrono::nanoseconds>(stop - start);
            insertion_time += duration.count();
        }

        if (verbose) {
            cout << "Tree type: " << tree_type << endl;
            cout << "Insertion time: " << insertion_time << " ns" << endl;
        }

        srand(time(0));

        long long query_time = 0;
        for (uint64_t i = 0; i < (uint64_t)N / 100; i++) {
            int random = rand() % (maxval - minval + 1) + minval;
            uint8_t key[ART::keyBytes];
            ART::loadKey(keys[random], key);
            auto start = chrono::high_resolution_clock::now();
            ART::ArtNode* leaf = tree->lookup(key);
            auto stop = chrono::high_resolution_clock::now();
            auto duration =
                chrono::duration_cast<chrono::nanoseconds>(stop - start);
            query_time += duration.count();
            assert(ART::isLeaf(leaf) &&
                   ART::getLeafValue(leaf) == keys[random]);
        }

        if (verbose) {
            cout << "Query time: " << query_time << " ns" << endl;
        }

        // Output the times in csv format, including tree type
        cout << insertion_time << "," << query_time << endl;
    } else {
        cerr << "Unknown tree type: " << tree_type << endl;
        return 1;
    }
    return 0;
}
