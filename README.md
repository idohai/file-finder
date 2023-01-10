# file-finder
Parallel search directory trees for files with a name containing a search term.  
Implemented with C using pthreads and filesystem calls


To compile:  
gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread pfind.c  

argv [1]: search root directory  
&emsp;&emsp; [2]: search term  
&emsp;&emsp; [3]: number of searching threads to be used

</br></br>

    
* Directory read and exec permissions are required to search a dir.
* Changing directory tree while program running may cause errors
