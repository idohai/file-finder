#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>

#define SUCCESS 0
#define FAIL 1

typedef struct node{
	char* dir_name;
	struct node* next;
} dir_node;

typedef struct linked_list{
	dir_node* head;
	dir_node* tail;
	int len;
} queue;

atomic_int threads_created_count = 0;
atomic_int threads_waiting_count = 0;
atomic_int threads_error_count = 0;
atomic_int files_found_count = 0;

pthread_mutex_t qlock;
pthread_mutex_t q_is_empty_lock;
pthread_mutex_t thread_creation_lock;
pthread_mutex_t all_threads_lock;

pthread_cond_t q_is_empty;
pthread_cond_t all_threads_created;

queue* q;
char* search_term;

int dir_is_searchable(struct stat stinfo){
	if (! (stinfo.st_mode & S_IRUSR)){
		return 0;
	}
	if (! (stinfo.st_mode & S_IXUSR)){
		return 0;
	}
	return 1;
}

void initialize_queue(char* dir_name){
	dir_node* root_dir;
	struct stat file_info;
	if (stat(dir_name, &file_info) == -1){
		fprintf(stderr, "stat system call failed with root_directory\n");
		exit(FAIL);
	}
	if (!S_ISDIR(file_info.st_mode)){
		fprintf(stderr, "root_directory path given is not a valid directory");
		exit(FAIL);
	}
	if (!dir_is_searchable(file_info)){
		printf ("Directory %s: Permission denied.\n", dir_name);
		exit(FAIL);
	}
	if (!(q = malloc(sizeof(queue)))){
		fprintf(stderr, "initialize_queue memory allocation failed on main thread");
		exit(FAIL);
	}
	if (!(root_dir = malloc(sizeof(dir_node)))){
		fprintf(stderr, "initialize_queue root_dir node memory allocation failed on main thread");
		exit(FAIL);
	}
	root_dir->dir_name = dir_name;
	root_dir->next = NULL;
	q->head = root_dir;
	q->tail = root_dir;
	q->len = 1;
}

void initialize_mutex(){
	pthread_mutex_init(&qlock, NULL);
	pthread_mutex_init(&q_is_empty_lock, NULL);
	pthread_mutex_init(&thread_creation_lock, NULL);
	pthread_mutex_init(&all_threads_lock, NULL);
	pthread_cond_init(&q_is_empty, NULL);
	pthread_cond_init(&all_threads_created, NULL);
}

int thread_creation_hold(){
	int result;
	pthread_mutex_lock(&thread_creation_lock);
	result = threads_created_count;
	pthread_mutex_unlock(&thread_creation_lock);
	return result;
}

int enqueue(char* full_path){
	dir_node* node;
	char* dir_name;
	if (!(dir_name = malloc(sizeof(char)*PATH_MAX))){
		fprintf(stderr, "memory allocation for new directory name failed within thread.");
		return 0;
	}
	if (!(node = malloc(sizeof(dir_node)))){
		fprintf(stderr, "memory allocation for new directory name failed within thread.");
		return 0;
	}
	strcpy(dir_name, full_path);
	node->dir_name = dir_name;
	node->next = NULL;
	pthread_mutex_lock(&qlock);
	if (q->len == 0){
		q->head = node;
		q->tail = node;
		q->len = 1;
		pthread_cond_signal(&q_is_empty);
	}
	else{
		q->tail->next = node;
		q->tail = node;
		q->len++;
	}
	pthread_mutex_unlock(&qlock);
	return 1;
}

dir_node* dequeue(){
	dir_node* head;
	head = q->head;
	if (q->len == 1){
		q->head = NULL;
		q->tail = NULL;
		q->len = 0;
	}
	else{
		q->head = head->next;
		q->len--;
	}
	return head;
}

int file_name_contains_search_term(char* file_name){
	if (strstr(file_name, search_term) == NULL){
		return 0; 
	}
	return 1;
}

int flow_of_search_thread(dir_node* current_directory){
	//using readdir(), iterarte through each directory entry (dirent):
	DIR* dirp;
	char* curr_dir_name;
	char* curr_file_name;
	char full_path[PATH_MAX];
	struct dirent* entry;
	struct stat file_info;

	curr_dir_name = current_directory->dir_name;
	dirp = opendir(curr_dir_name);
	if (dirp == NULL){
		//directory cant be searched.. then this directory won't be in queue.
		fprintf(stderr, "Directory opendir(%s) failed within thread", curr_dir_name);
		threads_error_count--;
		return 1; //error
	}

	//iterarting:
	while ( (entry = readdir(dirp)) ){
		curr_file_name = entry->d_name;
		//(a) if the name is "." or "..", ignore:
		if ((strcmp(curr_file_name, ".") == 0) || (strcmp(curr_file_name, "..") == 0)){
			continue;
		}
		sprintf(full_path, "%s/%s", curr_dir_name, curr_file_name);
		//(b) if the dirent is for a directory (check with stat()):
		if (stat(full_path, &file_info) == -1){
			fprintf(stderr, "stat system call failed with %s\n", full_path);
			threads_error_count++;
			break;
		}
		if (S_ISDIR(file_info.st_mode)){
			//if directory can be searched, add that to tail of queue
			if(dir_is_searchable(file_info)){
				if (!enqueue(full_path)){
					threads_error_count++;
				}
			}
			//if directory can't be searched, print message
			else{
				printf("Directory %s: Permission denied.\n", full_path);
			}
		}
		//(c) if dirent not directory and its name contains the search term
		else{
			if (file_name_contains_search_term(curr_file_name)){
				files_found_count++;
				printf("%s\n", full_path);
			}
		}
	}
	closedir(dirp);
	return 0;
}

void* search_thread(){
	int result;
	dir_node* current_directory;
	threads_created_count++;
	pthread_mutex_lock(&all_threads_lock);
	pthread_cond_wait(&all_threads_created, &all_threads_lock);
	pthread_mutex_unlock(&all_threads_lock);
	//now all threads was created. start search:
	result = 0;
	while (1){
		pthread_mutex_lock(&qlock);
		if (q->len == 0){ //if queue is empty:
	//wait until queue becomes non-empty. if all threads are waiting all threads should exit
			pthread_mutex_unlock(&qlock);
			threads_waiting_count++;
			pthread_mutex_lock(&q_is_empty_lock);
			pthread_cond_wait(&q_is_empty, &q_is_empty_lock);
			pthread_mutex_unlock(&q_is_empty_lock);
			threads_waiting_count--;
		}
		else{ //queue is not empty:
			// dequeue head directory from FIFO.
			current_directory = dequeue();
			pthread_mutex_unlock(&qlock);
			result = flow_of_search_thread(current_directory);
			if (result == 1){
				pthread_exit(NULL);
			}
		}
	}
}

int main (int argc, char* argv[]){
	int i;
	int num_of_threads;
	pthread_t* threads_id;
	if (argc != 4){
		fprintf(stderr, "Invalid arguments, should be root_directory, search_term, num_of_searching_threads\n");
		exit(FAIL);
	}
	search_term = argv[2];
	num_of_threads = atoi(argv[3]);
	
	// Create a FIFO queue that holds directories & put root_directory in queue
	// put root_directory in queue
	initialize_queue(argv[1]);
	initialize_mutex();

	// Create n searching threads
	if (!(threads_id = malloc(sizeof(pthread_t)*num_of_threads))){
		fprintf(stderr, "memory allocation for threads failed\n");
		exit(FAIL);
	}
	for (i=0; i<num_of_threads; i++){
		if (pthread_create(&threads_id[i],NULL, search_thread, NULL) != 0){
            fprintf(stderr, "Thread %d creation failed\n", i);
            exit(FAIL);
		}
	}

	// wait for all threads to be created, then main thread signal searching should start
	while (thread_creation_hold() < num_of_threads){};
	pthread_cond_broadcast(&all_threads_created);

	//finish control:
	while (1){
		if (threads_error_count + threads_waiting_count == num_of_threads){
			break;
		}
	}

	for (i=0; i<num_of_threads;i ++){
		pthread_cancel(threads_id[i]);
	}
	printf("Done searching, found %d files\n", files_found_count);
	if (threads_error_count > 0){
		exit(FAIL);
	}
	exit(SUCCESS);
}