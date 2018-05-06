#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <vector>
#include <list>
#include <set>
#include <unordered_map>
#include <numeric>
#include <cstddef>
#include <mpi.h>
#include <thread>
#include <pthread.h>

#include "kmer_t.hpp"
#include "hashmap_mpi.hpp"
#include "read_kmers.hpp"

int main(int argc, char **argv) {

	int n_proc = 1, rank = 0, provided;
	MPI_Init_thread( &argc, &argv, MPI_THREAD_SERIALIZED, &provided );
	MPI_Comm_size( MPI_COMM_WORLD, &n_proc );			    
	MPI_Comm_rank( MPI_COMM_WORLD, &rank );

	if (rank == 0) { 
		print ("SINGLE: %d FUNNELED: %d SERIALIZED: %d MULTIPLE: %d\n", MPI_THREAD_SINGLE, MPI_THREAD_FUNNELED, MPI_THREAD_SERIALIZED, MPI_THREAD_MULTIPLE); 
		print ("Only supported %d\n", provided);
	}

	std::string kmer_fname = std::string(argv[1]);
	std::string run_type = "";

	if (argc >= 3) {
		run_type = std::string(argv[2]);
	}
		
	uint64_t ks = kmer_size(kmer_fname);
				  
	if (ks != KMER_LEN) {			    
		throw std::runtime_error("Error: " + kmer_fname + " contains " +
				std::to_string(ks) + "-mers, while this binary is compiled for " +
				std::to_string(KMER_LEN) + "-mers.  Modify packing.hpp and recompile.");
	}
	
	uint64_t n_kmers = line_count(kmer_fname);
	
	if (rank == 0) {
		print("#### Total number of kmers: %d\n", n_kmers);
	}

	int bufsize = 0.5 * n_kmers * sizeof(MYMPI_Msg) / n_proc;
	void *bsend_buf = malloc(bufsize);
	MPI_Buffer_attach(bsend_buf, bufsize);
	
	uint64_t hash_table_size = n_kmers * (1.2);//(1.0 / 0.5);
	
	MYMPI_Hashmap hashmap(hash_table_size, n_proc, rank);
	
	if (run_type == "verbose" && rank == 0) {
		print("Initializing hash table of size %d for %d kmers.\n",
		hash_table_size, n_kmers);
	}

	std::vector <kmer_pair> kmers = read_kmers(kmer_fname, n_proc, rank);
	
	if (run_type == "verbose" && rank == 0) {
		print("Finished reading kmers.\n");
	}

	auto start = std::chrono::high_resolution_clock::now();

	std::vector <kmer_pair> start_nodes;

	std::thread t1(&MYMPI_Hashmap::collect_remote_insert, &hashmap);

	uint64_t outgoing = 0;
	for (auto &kmer : kmers) {
		//hashmap.sync_insert();

		bool success = hashmap.insert(kmer, outgoing);
		if (!success) {
			throw std::runtime_error("Error: HashMap is full!");
		}
		if (kmer.backwardExt() == 'F') {
			start_nodes.push_back(kmer);
		}
	}	

	print ("\t rank %d read %zu kmers, local insert %zu remote insert %zu\n", rank, kmers.size(), kmers.size() - outgoing, outgoing);
	
	pthread_cancel(t1.native_handle());	
	t1.join();
	uint64_t rsize = 0;
	while (rsize < n_kmers) {
			auto size = hashmap.size();
			MPI_Allreduce(&size, &rsize, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
			hashmap.collect_remote_insert();
	}
	MPI_Barrier(MPI_COMM_WORLD);

	assert(rsize == n_kmers);
	
	print("Rank: %d n_kmers: %zu hashmap size:%zu\n", rank, n_kmers, hashmap.size());
	
	auto end_insert = std::chrono::high_resolution_clock::now();
	
	double insert_time = std::chrono::duration <double> (end_insert - start).count();
	
	if (run_type != "test" && rank == 0) {
		print("Finished inserting in %lf\n", insert_time);
	}


	auto start_read = std::chrono::high_resolution_clock::now();
	std::vector <std::list <kmer_pair>> contigs;
	
	for (const auto &start_kmer : start_nodes) {
		contigs.emplace_back(std::list<kmer_pair> (1, start_kmer));
	}

	std::atomic<int> total_done = 0;
	uint64_t done_quest = 0;
	uint64_t quest = start_nodes.size();
	void *dummy_buf = malloc(quest * sizeof(bool));
  bool* ready = (bool*) dummy_buf;
	std::fill_n(ready, quest, true);
	uint64_t index = 0;
  if(rank == 0){
		print("rank 0 starting assembly\n");
  }
	

	std::atomic<int> isfinish = 0;
	//Check incoming MPI message
	
	std::thread t2(&MYMPI_Hashmap::communicate_messages, &hashmap, std::ref(contigs), std::ref(total_done), ready);

	std::thread t3(&MYMPI_Hashmap::deal_remote_messages, &hashmap, std::ref(contigs), std::ref(total_done), ready, std::ref(isfinish));
	
	while (total_done < n_proc) {
		
		if (done_quest < quest) {
			for (uint64_t i = 0; i < contigs.size(); i ++) {	
				if (!ready[i]) continue;

				if (contigs[i].back().forwardExt() == 'F') {
					ready[i] = false;
					done_quest ++;
					
					if (done_quest == quest) {
						broadcast_done(n_proc, rank, hashmap.messages);
						total_done ++;							
						break;
					}
				} else {
					kmer_pair kmer;
					bool is_local = hashmap.find(contigs[i].back().next_kmer(), kmer, ready, i);	
					if (is_local) {
						contigs[i].emplace_back(kmer);
					}
				}
			}
		}
	}
	isfinish = 1;
	print ("Finish Assembly\n");
	pthread_cancel(t2.native_handle());
	t2.join();
	pthread_cancel(t3.native_handle());
	t3.join();

	MPI_Barrier( MPI_COMM_WORLD );
	
	auto end_read = std::chrono::high_resolution_clock::now();
	
	auto end = std::chrono::high_resolution_clock::now();

	std::chrono::duration <double> read = end_read - start_read;
	std::chrono::duration <double> insert = end_insert - start;
	std::chrono::duration <double> total = end - start;


	uint64_t numKmers = std::accumulate(contigs.begin(), contigs.end(), 0,
			[] (uint64_t sum, const std::list <kmer_pair> &contig) {
				return sum + contig.size(); 
			});

	  if (run_type != "test" && rank == 0) {
			print("Assembled in %lf total\n", total.count());
		}
		
		if (run_type == "verbose" && rank == 0) {
			printf("Rank %d reconstructed %d contigs with %d nodes from %d start nodes."
					" (%lf read, %lf insert, %lf total)\n", rank, contigs.size(), numKmers, start_nodes.size(), read.count(), insert.count(), total.count());
		}
			  
		if (run_type == "test") {
			std::ofstream fout("test_" + std::to_string(rank) + ".dat");
			for (const auto &contig : contigs) {
				fout << extract_contig(contig) << std::endl;
			}
			fout.close();
		}
	
	
	MPI_Buffer_detach(&bsend_buf, &bufsize);
	free(bsend_buf);
    free(dummy_buf);
	MPI_Finalize( );

	return 0;
}
