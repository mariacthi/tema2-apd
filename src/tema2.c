#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define INIT_TAG 0
#define REQUEST_TAG 1
#define ANSWER_TAG 2
#define UPLOAD_TAG 3
#define ACK_TAG 4

// Un segment este definit de un hash si de pozitia sa in fisier."
typedef struct {
    char hash[HASH_SIZE + 1];
    int position;
} chunk_info;

// "Swarmul unui fisier contine lista de clienti (seeds sau peers) care
// au segmente din fisierul respectiv, dar nu si ce segmente are fiecare client."
typedef struct {
    int clients[MAX_FILES];
    int types[MAX_FILES]; // 0 - peer, 1 - seed
} swarm_entry;

typedef struct {
    char filename[MAX_FILENAME];
    chunk_info chunks[MAX_CHUNKS];
    int chunks_count;
    int chunks_recv_from[MAX_CHUNKS];
} file_info;

file_info files_owned[MAX_FILES];
int files_owned_count = 0;

file_info files_requested[MAX_FILES];
int files_requested_count = 0;
int files_requested_completed = 0;

//  "Trackerul mentine o lista de fisiere si swarmul asociat fiecaruia dintre ele."
typedef struct {
    file_info file;
    swarm_entry swarm;
    int swarm_count;
} tracker_info;

tracker_info tracker_database[MAX_FILES];
int files_count = 0;

void write_file(int rank, char *filename, char *hash) {
    char output[30];
    snprintf(output, MAX_FILENAME, "client%d_%s", rank, filename);

    FILE *output_file = fopen(output, "a");
    if (output_file == NULL) {
        perror("Opening file failed");
        exit(-1);
    }

    fwrite(hash, sizeof(char), HASH_SIZE, output_file);
    fwrite("\n", sizeof(char), 1, output_file);
    fclose(output_file);
}

int is_owner_used(int owner, file_info *file, int chunk_index) {
    for (int i = 0; i < file->chunks_count; i++) {
        // verifica daca owner-ul a fost deja utilizat pentru acest chunk
        if (file->chunks[i].position == chunk_index && file->chunks_recv_from[i] == owner) {
            return 1;
        }
    }
    return 0; // owner-ul nu a fost utilizat
}

int choose_owner(int owner, int swarm_count, swarm_entry swarm, int rank, int file_index, int i) {
    int attempts = 0;

    while (owner == -1 && attempts < swarm_count) {
        // daca nu a gasit un owner, genereaza un index aleator din lista de clienti
        int random_index = rand() % swarm_count;
        int potential_owner = swarm.clients[random_index];

        // verifica daca acest owner este folosit sau este chiar clientul curent
        if (!is_owner_used(potential_owner, &files_requested[file_index], i) && potential_owner != rank) {
            owner = potential_owner;
        }

        attempts++;
    }

    // daca nu a gasit un owner nou, folosește primul client din lista
    if (owner == -1) {
        owner = swarm.clients[0];
    }

    return owner;
}

swarm_entry request_swarm(int *swarm_count, char *filename, char *request) {
    // trimite cerere la tracker pentru swarm
    MPI_Send(request, 15, MPI_CHAR, TRACKER_RANK, REQUEST_TAG, MPI_COMM_WORLD);
    MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_TAG, MPI_COMM_WORLD);

    // primeste numarul de fisiere din swarm
    MPI_Recv(swarm_count, 1, MPI_INT, TRACKER_RANK, ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    swarm_entry swarm;
    for (int i = 0; i < *swarm_count; i++) {
        // primeste informatii despre fiecare client din swarm
        MPI_Recv(&swarm.clients[i], 1, MPI_INT, TRACKER_RANK, ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(&swarm.types[i], 1, MPI_INT, TRACKER_RANK, ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    return swarm;
}

swarm_entry update(int *swarm_count, char *filename) {
    // "Actualizare. [..]
    // Dupa fiecare 10 segmente descarcate de la seeds/peers:
    // 1. ii cere trackerului lista actualizata de seeds/peers pentru fisierele pe
    // care le doreste
    return request_swarm(swarm_count, filename, "UPDATE");
    // 2. reia de la pasul 2 al sectiunii de Descarcare"
}

void receive_chunks_info(int *chunks_count, chunk_info *chunks) {
    // primeste numarul de segmente  de la tracker
    MPI_Recv(chunks_count, 1, MPI_INT, TRACKER_RANK, ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for(int i = 0; i < *chunks_count; i++) {
        // primeste hash-ul fiecarui segment de la tracker
        MPI_Recv(chunks[i].hash, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, ANSWER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        chunks[i].position = i;
    }
}

int download_file_requested(int rank, char *filename, int file_index) {
    // "Descarcare. Odata ce a primit confirmarea de la tracker ca poate continua, clientul
    // realizeaza urmatorii pasi:
    // 1. ii cere trackerului lista de seeds/peers pentru fisierele pe care le doreste (cu
    // alte cuvinte, swarmurile fisierelor)
    int swarm_count;
    swarm_entry swarm = request_swarm(&swarm_count, filename, "REQUEST SWARM");

    chunk_info chunks[MAX_CHUNKS];
    int chunks_count;
    receive_chunks_info(&chunks_count, chunks);

    //  2. se uita la segmentele care ii lipsesc din fisierele pe care le doreste,
    // cauta in swarmul de la tracker, si incepe sa trimita cereri catre peers/seeds
    int owner = -1;
    for(int i = 0; i < chunks_count; i++) {
        if (i % 10 == 0 && i != 0) {
            swarm = update(&swarm_count, filename);
        }

        //  3. pentru fiecare segment dintr-un fisier dorit pe care nu il detine, un client
        // realizeaza urmatorii pasi:
        // (a) cauta un seed/peer care detine segmentul cautat
        owner = choose_owner(owner, swarm_count, swarm, rank, file_index, i);

        // (b) ii trimite acelui seed/peer o cerere pentru segment
        char request[15] = "UPLOAD REQUEST";
        MPI_Send(request, 15, MPI_CHAR, owner, UPLOAD_TAG, MPI_COMM_WORLD);
        // clientul trimite numele fisierului, hash-ul segmentului si pozitia segmentului
        MPI_Send(files_requested[file_index].filename, MAX_FILENAME, MPI_CHAR, owner, UPLOAD_TAG, MPI_COMM_WORLD);
        MPI_Send(chunks[i].hash, HASH_SIZE + 1, MPI_CHAR, owner, UPLOAD_TAG, MPI_COMM_WORLD);
        MPI_Send(&chunks[i].position, 1, MPI_INT, owner, UPLOAD_TAG, MPI_COMM_WORLD);

            // (c) asteapta sa primeasca de la seed/peer segmentul cerut
        char response[15];
        MPI_Recv(response, 15, MPI_CHAR, owner, ACK_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (strcmp(response, "ACK") == 0) {
            // (d) marcheaza segmentul ca primit"
            files_requested[file_index].chunks_recv_from[files_requested[file_index].chunks_count] = owner;
            strcpy(files_requested[file_index].chunks[files_requested[file_index].chunks_count].hash, chunks[i].hash);
            files_requested[file_index].chunks[files_requested[file_index].chunks_count++].position = chunks[i].position;

            // reseteaza owner-ul pentru urmatorul segment
            owner = -1;
        } else if (strcmp(response, "NACK") == 0) {
            // daca nu a primit segmentul, reseteaza owner-ul la cel default pentru a fi sigur
            // ca il primeste la urmatoarea iteratie
            owner = swarm.clients[0];

            i--;
        }
    }

    return chunks_count;
}

void *download_thread_func(void *arg) {
    int rank = *(int*) arg;

    while(files_requested_completed < files_requested_count) {
        for(int i = 0; i < files_requested_count; i++) {
            int chunks_count = download_file_requested(rank, files_requested[i].filename, i);

            if(files_requested[i].chunks_count == chunks_count) {
                // "Finalizare descarcare fisier. Atunci cand termina de descarcat toate
                // segmentele unui fisier, un client realizeaza urmatorii pasi:
                // 1. informeaza trackerul ca are tot fisierul
                char response[15] = "DOWNLOADED";
                MPI_Send(response, 15, MPI_CHAR, TRACKER_RANK, REQUEST_TAG, MPI_COMM_WORLD);
                MPI_Send(files_requested[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_TAG, MPI_COMM_WORLD);

                for (int j = 0; j < files_requested[i].chunks_count; j++) {
                    // 2. salveaza fisierul (lista de hash-uri in ordine) intr-un fisier de
                    // iesire numit client<R> <NUMEFISIER>"
                    write_file(rank, files_requested[i].filename, files_requested[i].chunks[j].hash);
                }

                files_requested_completed++;
            }
        }
    }
    // "Finalizare descarcare toate fisierele. Atunci cand termina de descarcat toate
    // fisierele pe care le dorea, un client realizeaza urmatorii pasi:
    // 1. ii trimite trackerului un mesaj prin care il informeaza ca a terminat toate descarcarile
    char request[15] = "FINISH";
    MPI_Send(request, 15, MPI_CHAR, TRACKER_RANK, REQUEST_TAG, MPI_COMM_WORLD);
    // 2. inchide firul de executie de download (download thread), dar il lasa deschis pe cel de upload"
    return NULL;
}

void handle_upload_request(MPI_Status status) {
    // "Primire de cereri de segmente de la alti clienti. Atunci cand primeste o cerere de
    // segment de la un peer, un client verifica intai daca detine acel segment."

    char filename[MAX_FILENAME];
    int position;
    char hash[HASH_SIZE + 1];

    // primeste numele fisierului, hash-ul segmentului si pozitia segmentului de la client
    MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Recv(hash, HASH_SIZE + 1, MPI_CHAR, status.MPI_SOURCE, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Recv(&position, 1, MPI_INT, status.MPI_SOURCE, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    int found = 0;
    for(int i = 0; i < files_owned_count; i++) {
        // verifica ca numele fisierului si hash-ul segmentului sa fie identice
        if (strcmp(files_owned[i].filename, filename) == 0 &&
            strcmp(files_owned[i].chunks[position].hash, hash) == 0) {
            // "Daca il detine, ii “trimite” peerului segmentul cerut"
            char response[15] = "ACK";
            MPI_Send(response, 15, MPI_CHAR, status.MPI_SOURCE, ACK_TAG, MPI_COMM_WORLD);
            found = 1;
            break;
        }
    }

    if (found == 0) {
        // "Daca nu il detine, ii trimite peerului un raspuns negativ"
        char response[15] = "NACK";
        MPI_Send(response, 15, MPI_CHAR, status.MPI_SOURCE, ACK_TAG, MPI_COMM_WORLD);
    }
}

void *upload_thread_func(void *arg) {
    //int rank = *(int*) arg;

    while (1) {
        MPI_Status status;
        char request[15] = "";

        // asteapta sa primeasca cerere de la un client
        MPI_Recv(request, 15, MPI_CHAR, MPI_ANY_SOURCE, UPLOAD_TAG, MPI_COMM_WORLD, &status);

        if (strcmp(request, "UPLOAD REQUEST") == 0) {
            handle_upload_request(status);
        }

        // daca primeste mesaj de finalizare de la tracker, se opreste
        if (strcmp(request, "DONE") == 0) {
            return NULL;
        }


    }
}

void initialise_tracker(int numtasks, int rank) {
    int files_owned, chunks;

    // "Initializare. La initializare, trackerul realizeaza urmatorii pasi:
    for(int i = 1; i < numtasks; i++) {
        //  1. asteapta mesajul initial al fiecarui client, care va contine
        // lista de fisiere detinute
        MPI_Recv(&files_owned, 1, MPI_INT, i, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        for(int j = 0; j < files_owned; j++) {
            MPI_Recv(tracker_database[files_count].file.filename, MAX_FILENAME, MPI_CHAR, i, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(&chunks, 1, MPI_INT, i, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            tracker_database[files_count].file.chunks_count = 0;
            for(int k = 0; k < chunks; k++) {
                // formeaza structura pentru fiecare segment cu hash-ul primit de la client
                // si pozitia segmentului (ele se trimit in ordine)
                chunk_info current_chunk;
                MPI_Recv(current_chunk.hash, HASH_SIZE + 1, MPI_CHAR, i, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                current_chunk.position = k;

                tracker_database[files_count].file.chunks[tracker_database[files_count].file.chunks_count++] = current_chunk;
            }

            // 2. pentru fiecare fisier detinut de un client, trece clientul
            // respectiv ca seed in swarmul fisierului
            tracker_database[files_count].swarm_count = 0;
            tracker_database[files_count].swarm.types[tracker_database[files_count].swarm_count] = 1;
            tracker_database[files_count].swarm.clients[tracker_database[files_count].swarm_count++] = i;

            files_count++;
        }
    }


    // 3. cand a primit de la toti clientii, raspunde fiecaruia cu cate un “ACK”"
    for(int i = 1; i < numtasks; i++) {
        char response[15] = "INIT OK";
        MPI_Send(response, 15, MPI_CHAR, i, INIT_TAG, MPI_COMM_WORLD);
    }
}

void send_done_message(int numtasks) {
    char message[15] = "DONE";
    for(int i = 1; i < numtasks; i++) {
        MPI_Send(message, 15, MPI_CHAR, i, UPLOAD_TAG, MPI_COMM_WORLD);
    }
}

void tracker(int numtasks, int rank) {
    initialise_tracker(numtasks, rank);

    int clients_ready[numtasks];
    for(int i = 0; i < numtasks; i++) {
        clients_ready[i] = 0;
    }

    while(1) {
        char request[15];
        MPI_Status status;

        // "Primire de mesaje de la clienti. Atunci cand primeste un mesaj de la un client,
        // trackerul realizeaza urmatorii pasi:
        // 1. se uita ce fel de mesaj a primit
        MPI_Recv(request, 15, MPI_CHAR, MPI_ANY_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, &status);

        if (strcmp(request, "REQUEST SWARM") == 0) {
            // "daca mesajul primit este o cerere de la un client pentru un fisier (sau o
            // cerere de actualizare), trackerul ii da acestuia lista cu clientii seeds/peers
            // din swarm, si marcheaza clientul ca peer pentru acel fisier (deci  il si adauga
            // implicit in swarmul fisierului)""

            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for(int i = 0; i < files_count; i++) {
                // cauta fisierul in database-ul tracker-ului
                if (strcmp(tracker_database[i].file.filename, filename) == 0) {

                    // trimite clientului numarul de clienti din swarm
                    MPI_Send(&tracker_database[i].swarm_count, 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                    for (int j = 0; j < tracker_database[i].swarm_count; j++) {
                        // trimite clientului fiecare client din swarm si tipul acestuia (seed/peer)
                        MPI_Send(&tracker_database[i].swarm.clients[j], 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                        MPI_Send(&tracker_database[i].swarm.types[j], 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                    }
                    // mesajul este cerere de la un client pentru un fisieer, deci tracker-ul va trimite
                    // si hash-urile segmentelor fisierului
                    MPI_Send(&tracker_database[i].file.chunks_count, 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                    for (int j = 0; j < tracker_database[i].file.chunks_count; j++) {
                        MPI_Send(tracker_database[i].file.chunks[j].hash, HASH_SIZE + 1, MPI_CHAR, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                    }

                    // marcheaza clientul ca peer pentru fisier
                    tracker_database[i].swarm.types[tracker_database[i].swarm_count] = 0;
                    tracker_database[i].swarm.clients[tracker_database[i].swarm_count++] = status.MPI_SOURCE;

                    break;
                }
            }
        }

        if (strcmp(request, "UPDATE") == 0) {
            // cerere de actualizare a swarm-ului (va trimite doar swarm-ul fisierului)
            char filename_req[MAX_FILENAME];
            MPI_Recv(filename_req, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for(int i = 0; i < files_count; i++) {
                // cauta fisierul in database-ul tracker-ului
                if (strcmp(tracker_database[i].file.filename, filename_req) == 0) {
                    // trimite clientului numarul de clienti din swarm
                    MPI_Send(&tracker_database[i].swarm_count, 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                    for (int j = 0; j < tracker_database[i].swarm_count; j++) {
                        // trimite clientului fiecare client din swarm si tipul acestuia (seed/peer)
                        MPI_Send(&tracker_database[i].swarm.clients[j], 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                        MPI_Send(&tracker_database[i].swarm.types[j], 1, MPI_INT, status.MPI_SOURCE, ANSWER_TAG, MPI_COMM_WORLD);
                    }
                    break;
                }
            }
        }

        if (strcmp(request, "DOWNLOADED") == 0) {
            // daca mesajul primit este de finalizare a unei descarcari de fisier, trackerul
            // marcheaza clientul respectiv ca seed
            char filename_req[MAX_FILENAME];
            MPI_Recv(filename_req, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for(int i = 0; i < files_count; i++) {
                if (strcmp(tracker_database[i].file.filename, filename_req) == 0) {
                    for (int j = 0; j < tracker_database[i].swarm_count; j++) {
                        if (tracker_database[i].swarm.clients[j] == status.MPI_SOURCE) {
                            tracker_database[i].swarm.types[j] = 1;
                            break;
                        }
                    }
                }
            }

        }

        if(strcmp(request, "FINISH") == 0) {
            // daca mesajul primit este de finalizare a descarcarii tuturor fisierelor de
            // catre un client, trackerul marcheaza clientul respectiv ca terminat, dar il
            // tine in swarmurile fisierelor pe care le detine
            clients_ready[status.MPI_SOURCE] = 1;

            int finish = 1;
            for(int i = 1; i < numtasks; i++) {
                if(clients_ready[i] == 0) {
                    finish = 0;
                    break;
                }
            }

            if(finish == 1) {
                // daca mesajul primit semnifica faptul ca toti clientii au terminat de
                // descarcat toate fisierele dorite, trackerul trimite cate un mesaj de
                // finalizare catre fiecare client, pentru a le spune sa se opreasca,
                // dupa care se opreste si el
                send_done_message(numtasks);
                return;
            }

        }
    }
}

void read_input_file(int rank) {
    //  "Fiecare client va avea cate un fisier de intrare denumit in<R>.txt
    // (unde R este rangul taskului)"
    char file[MAX_FILENAME];
    snprintf(file, MAX_FILENAME, "in%d.txt", rank);

    FILE *input = fopen(file, "r");
    if (input == NULL) {
        printf("Input file error\n");
        exit(-1);
    }

    // citeste numarul de fisiere detinute
    fscanf(input, "%d", &files_owned_count);
    for(int i = 0; i < files_owned_count; i++) {
        // citeste numele fisierului
        fscanf(input, "%s", files_owned[i].filename);

        // citeste numarul de segmente
        fscanf(input, "%d", &files_owned[i].chunks_count);
        for(int j = 0; j < files_owned[i].chunks_count; j++) {
            // citeste hash-ul segmentului
            fscanf(input, "%s", files_owned[i].chunks[j].hash);
        }
        for (int j = 0; j < MAX_CHUNKS; j++) {
            // initializeaza owner-ul fiecarui chunk cu -1 (nu are owner)
            files_owned[i].chunks_recv_from[j] = -1;
        }
    }

    // citeste numarul de fisiere cerute
    fscanf(input, "%d", &files_requested_count);
    for(int i = 0; i < files_requested_count; i++) {
        // citeste numele fisierului
        fscanf(input, "%s", files_requested[i].filename);

        for(int j = 0; j < MAX_CHUNKS; j++) {
            // initializeaza owner-ul fiecarui chunk cu -1 (nu are owner)
            files_requested[i].chunks_recv_from[j] = -1;
        }
    }

    fclose(input);
}

void send_to_tracker() {
    // "trackerul va sti care sunt fisierele din retea, cate segmente au, in ce ordine sunt,
    // ce hash-uri au, si cine le detine initial"

    // trimite numarul de fisiere detinute
    MPI_Send(&files_owned_count, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
    for(int i = 0; i < files_owned_count; i++) {
        // trimite numele fisierului
        MPI_Send(files_owned[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);

        // trimite numarul de segmente
        MPI_Send(&files_owned[i].chunks_count, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
        for(int j = 0; j < files_owned[i].chunks_count; j++) {
            // "pentru fiecare fisier, clientul ii transmite trackerului hash-ul fiecarui
            // segment, in ordine"
            MPI_Send(files_owned[i].chunks[j].hash, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
        }
    }
}

void initialise_client(int rank) {
    //  "Initializare. La initializare, un client realizeaza urmatorii pasi:

    // 1. citeste fisierul de intrare
    read_input_file(rank);

    //  2. ii spune trackerului ce fisiere are
    send_to_tracker();

    // 3.  asteapta un raspuns de la tracker ca poate incepe cautarea si descarcarea
    // fisierelor de care este interesat."
    char response[15];
    MPI_Recv(response, 15, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    if (strcmp(response, "INIT OK") != 0) {
        printf("Error at initialising client %d\n", rank);
        exit(-1);
    }
}

void peer(int numtasks, int rank) {
    initialise_client(rank);

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}

int main (int argc, char *argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
