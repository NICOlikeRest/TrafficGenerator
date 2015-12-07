#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pthread.h>

#include "../common/common.h"
#include "../common/cdf.h"
#include "../common/conn.h"

#ifndef max
    #define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
    #define min(a,b) ((a) < (b) ? (a) : (b))
#endif

struct Flow
{
    struct Conn_Node* node;
    unsigned int flow_id;
    unsigned int flow_size;
    unsigned int flow_tos;
    unsigned int flow_rate;
};

int debug_mode = 0; //debug mode (0 is inactive)

char config_file_name[80] = {'\0'}; //configuration file name
char dist_file_name[80] = {'\0'};   //size distribution file name
char log_prefix[] = "log"; //default
char fct_log_suffix[] = "flows.txt";
char rct_log_suffix[] = "reqs.txt";
char rct_log_name[80] = {'\0'}; //request completion times (RCT) log file name
char fct_log_name[80] = {'\0'};    //request flow completion times (FCT) log file name
int seed = 0; //random seed
int usleep_overhead_us = 0; //usleep overhead
struct timeval tv_start, tv_end; //start and end time of traffic

/* per-server variables */
int num_server = 0; //total number of servers
int *server_port = NULL;    //ports of servers
char (*server_addr)[20] = NULL; //IP addresses of servers
int *server_flow_count = NULL;  //the number of flows generated by each server

int num_fanout = 0; //Number of fanouts
int *fanout_size = NULL;
int *fanout_prob = NULL;
int fanout_prob_total = 0;
int max_fanout_size = 1;

int num_service = 0; //Number of services
int *service_dscp = NULL;
int *service_prob = NULL;
int service_prob_total = 0;

int num_rate = 0; //Number of sending rates
int *rate_value = NULL;
int *rate_prob = NULL;
int rate_prob_total = 0;

double load = 0; //Network load (Mbps)
int req_total_num = 0; //Total number of requests
int flow_total_num = 0; //Total number of flows (each request consists of several flows)
struct CDF_Table* req_size_dist = NULL;
int period_us;  //Average request arrival interval (us)

/* per-request variables */
int *req_size = NULL;   //request size
int *req_fanout = NULL; //request fanout size
int **req_server_flow_count = NULL; //number of flows (of this request) generated by each server
int *req_dscp = NULL;   //DSCP of request
int *req_rate = NULL;   //sending rate of request
int *req_sleep_us = NULL; //sleep time interval
struct timeval *req_start_time = NULL; //start time of request
struct timeval *req_stop_time = NULL;  //stop time of request

/* per-flow variables */
int *flow_req_id = NULL;    //request ID of the flow (a request has several flows)
struct timeval *flow_start_time = NULL; //start time of flow
struct timeval *flow_stop_time = NULL;  //stop time of flow

struct Conn_List* connection_lists = NULL; //connection pool
int global_flow_id = 0; //flow ID

/* Print usage of the program */
void print_usage(char *program);
/* Read command line arguments */
void read_args(int argc, char *argv[]);
/* Read configuration file */
void read_config(char *file_name);
/* Set request variables */
void set_req_variables();
/* Receive traffic from established connections */
void *listen_connection(void *ptr);
/* Generate incast requests */
void run_requests();
/* Generate a incast request to some servers */
void run_request(unsigned int req_id);
/* Generate a flow request to a server */
void *run_flow(void *ptr);
/* Terminate all existing connections */
void exit_connections();
/* Terminate a connection */
void exit_connection(struct Conn_Node* node);
/* Print statistic data */
void print_statistic();
/* Clean up resources */
void cleanup();

int main(int argc, char *argv[])
{
    int i = 0;
    struct Conn_Node* ptr = NULL;

    /* read program arguments */
    read_args(argc, argv);

    /* set seed value for random number generation */
    if (seed == 0)
    {
        gettimeofday(&tv_start, NULL);
        srand((tv_start.tv_sec*1000000) + tv_start.tv_usec);
    }
    else
        srand(seed);

    /* read configuration file */
    read_config(config_file_name);
    /* Set request variables */
    set_req_variables();

    /* Calculate usleep overhead */
    usleep_overhead_us = get_usleep_overhead(10);
    printf("===============================\n");
    printf("The usleep overhead is %u us.\n", usleep_overhead_us);
    printf("===============================\n");

    connection_lists = (struct Conn_List*)malloc(num_server * sizeof(struct Conn_List));
    if (!connection_lists)
    {
        cleanup();
        error("Error: malloc");
    }

    /* Initialize connection pool and establish connections to servers */
    for (i = 0; i < num_server; i++)
    {
        if (!Init_Conn_List(&connection_lists[i], i, server_addr[i], server_port[i]))
        {
            cleanup();
            error("Error: Init_Conn_List");
        }
        if (!Insert_Conn_List(&connection_lists[i], max(max_fanout_size, TG_PAIR_INIT_CONN)))
        {
            cleanup();
            error("Error: Insert_Conn_List");
        }
        if (debug_mode)
            Print_Conn_List(&connection_lists[i]);
    }

    /* Start threads to receive traffic */
    for (i = 0; i < num_server; i++)
    {
        ptr = connection_lists[i].head;
        while (true)
        {
            if (!ptr)
                break;
            else
            {
                pthread_create(&(ptr->thread), NULL, listen_connection, (void*)ptr);
                ptr = ptr->next;
            }
        }
    }

    printf("Start to generate requests\n");
    printf("===============================\n");
    gettimeofday(&tv_start, NULL);
    global_flow_id =  0;
    run_requests();

    /* Close existing connections */
    exit_connections();
    gettimeofday(&tv_end, NULL);
    printf("Terminate connections\n");
    printf("===============================\n");

    /* Wait for all threads to finish */
    for (i = 0; i < num_server; i++)
        Wait_Conn_List(&connection_lists[i]);

    print_statistic();

    /* Release resources */
    cleanup();
    return 0;
}

/* Print usage of the program */
void print_usage(char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("-c <file>    name of configuration file (required)\n");
    printf("-l <prefix>  log file name prefix (default %s)\n", log_prefix);
    printf("-s <seed>    random seed value (default current system time)\n");
    printf("-d           debug mode (print necessary information)\n");
    printf("-h           display help information\n");
}

/* Read command line arguments */
void read_args(int argc, char *argv[])
{
    int i = 1;

    if (argc == 1)
    {
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
    }

    sprintf(fct_log_name, "%s_%s", log_prefix, fct_log_suffix);
    sprintf(rct_log_name, "%s_%s", log_prefix, rct_log_suffix);

    while (i < argc)
    {
        if (strlen(argv[i]) == 2 && strcmp(argv[i], "-c") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) < sizeof(config_file_name))
            {
                sprintf(config_file_name, "%s", argv[i+1]);
                i += 2;
            }
            /* cannot read IP address */
            else
            {
                printf("Cannot read configuration file name\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-l") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) + 1 + strlen(fct_log_suffix) < sizeof(fct_log_name) && strlen(argv[i+1]) + 1 + strlen(rct_log_suffix) < sizeof(rct_log_name))
            {
                sprintf(fct_log_name, "%s_%s", argv[i+1], fct_log_suffix);
                sprintf(rct_log_name, "%s_%s", argv[i+1], rct_log_suffix);
                i += 2;
            }
            /* cannot read IP address */
            else
            {
                printf("Cannot read log file prefix\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-s") == 0)
        {
            if (i+1 < argc)
            {
                seed = atoi(argv[i+1]);
                i += 2;
            }
            /* cannot read port number */
            else
            {
                printf("Cannot read seed value\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-d") == 0)
        {
            debug_mode = 1;
            i++;
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        else
        {
            printf("Invalid option %s\n", argv[i]);
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
}

/* Read configuration file */
void read_config(char *file_name)
{
    FILE *fd = NULL;
    char line[256] = {'\0'};
    char key[80] = {'\0'};
    num_server = 0;    //Number of senders
    int num_load = 0;   //Number of network loads
    int num_req = 0;    //Number of requests
    int num_dist = 0;   //Number of flow size distributions
    num_fanout = 0; //Number of fanouts (optinal)
    num_service = 0; //Number of services (optional)
    num_rate = 0; //Number of sending rates (optional)

    printf("===============================\n");
    printf("Reading configuration file %s\n", file_name);
    printf("===============================\n");

    /* Parse configuration file for the first time */
    fd = fopen(file_name, "r");
    if (!fd)
        error("Error: fopen");

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        sscanf(line, "%s", key);
        if (!strcmp(key, "server"))
            num_server++;
        else if (!strcmp(key, "load"))
            num_load++;
        else if (!strcmp(key, "num_reqs"))
            num_req++;
        else if (!strcmp(key, "req_size_dist"))
            num_dist++;
        else if (!strcmp(key, "fanout"))
            num_fanout++;
        else if (!strcmp(key, "service"))
            num_service++;
        else if (!strcmp(key, "rate"))
            num_rate++;
        else
            error("Error: invalid key in configuration file");
    }

    fclose(fd);

    if (num_server < 1)
        error("Error: configuration file should provide at least one server");
    if (num_load != 1)
        error("Error: configuration file should provide one network load");
    if (num_req != 1)
        error("Error: configuration file should provide one total number of requests");
    if (num_dist != 1)
        error("Error: configuration file should provide one request size distribution");

    /* Initialize configuration */
    /* per-server variables*/
    server_port = (int*)malloc(num_server * sizeof(int));
    server_addr = (char (*)[20])malloc(num_server * sizeof(char[20]));
    server_flow_count = (int*)calloc(num_server, sizeof(int));  //initialize as 0
    /* fanout size and probability */
    fanout_size = (int*)malloc(max(num_fanout, 1) * sizeof(int));
    fanout_prob = (int*)malloc(max(num_fanout, 1) * sizeof(int));
    /* service DSCP and probability */
    service_dscp = (int*)malloc(max(num_service, 1) * sizeof(int));
    service_prob = (int*)malloc(max(num_service, 1) * sizeof(int));
    /* sending rate value and probability */
    rate_value = (int*)malloc(max(num_rate, 1) * sizeof(int));
    rate_prob = (int*)malloc(max(num_rate, 1) * sizeof(int));

    if (!server_port || !server_addr || !server_flow_count || !fanout_size || !fanout_prob || !service_dscp || !service_prob || !rate_value || !rate_prob)
    {
        cleanup();
        error("Error: malloc");
    }

    /* Second time */
    num_server = 0;
    num_fanout = 0;
    num_service = 0;
    num_rate = 0;

    fd = fopen(file_name, "r");
    if (!fd)
    {
        cleanup();
        error("Error: fopen");
    }

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        remove_newline(line);
        sscanf(line, "%s", key);

        if (!strcmp(key, "server"))
        {
            sscanf(line, "%s %s %d", key, server_addr[num_server], &server_port[num_server]);
            if (debug_mode)
                printf("Server[%d]: %s, Port: %d\n", num_server, server_addr[num_server], server_port[num_server]);
            num_server++;
        }
        else if (!strcmp(key, "load"))
        {
            sscanf(line, "%s %lfMbps", key, &load);
            if (debug_mode)
                printf("Network Load: %.2f Mbps\n", load);
        }
        else if (!strcmp(key, "num_reqs"))
        {
            sscanf(line, "%s %d", key, &req_total_num);
            if (debug_mode)
                printf("Number of Requests: %d\n", req_total_num);
        }
        else if (!strcmp(key, "req_size_dist"))
        {
            sscanf(line, "%s %s", key, dist_file_name);
            if (debug_mode)
                printf("Loading request size distribution: %s\n", dist_file_name);

            req_size_dist = (struct CDF_Table*)malloc(sizeof(struct CDF_Table));
            if (!req_size_dist)
            {
                cleanup();
                error("Error: malloc");
            }

            init_CDF(req_size_dist);
            load_CDF(req_size_dist, dist_file_name);
            if (debug_mode)
            {
                printf("===============================\n");
                print_CDF(req_size_dist);
                printf("Average request size: %.2f bytes\n", avg_CDF(req_size_dist));
                printf("===============================\n");
            }
        }
        else if (!strcmp(key, "fanout"))
        {
            sscanf(line, "%s %d %d", key, &fanout_size[num_fanout], &fanout_prob[num_fanout]);
            if (fanout_size[num_fanout] < 1)
            {
                cleanup();
                error("Illegal fanout size");
            }
            else if(fanout_prob[num_fanout] < 0)
            {
                cleanup();
                error("Illegal fanout probability value");
            }

            fanout_prob_total += fanout_prob[num_fanout];
            if (fanout_size[num_fanout] > max_fanout_size)
                max_fanout_size = fanout_size[num_fanout];

            if (debug_mode)
                printf("Fanout: %d, Prob: %d\n", fanout_size[num_fanout], fanout_prob[num_fanout]);
            num_fanout++;
        }
        else if (!strcmp(key, "service"))
        {
            sscanf(line, "%s %d %d", key, &service_dscp[num_service], &service_prob[num_service]);
            if (service_dscp[num_service] < 0 || service_dscp[num_service] >= 64)
            {
                cleanup();
                error("Illegal DSCP value");
            }
            else if (service_prob[num_service] < 0)
            {
                cleanup();
                error("Illegal DSCP probability value");
            }
            service_prob_total += service_prob[num_service];
            if (debug_mode)
                printf("Service DSCP: %d, Prob: %d\n", service_dscp[num_service], service_prob[num_service]);
            num_service++;
        }
        else if (!strcmp(key, "rate"))
        {
            sscanf(line, "%s %dMbps %d", key, &rate_value[num_rate], &rate_prob[num_rate]);
            if (rate_value[num_rate] < 0)
            {
                cleanup();
                error("Illegal sending rate value");
            }
            else if (rate_prob[num_rate] < 0)
            {
                cleanup();
                error("Illegal sending rate probability value");
            }
            rate_prob_total += rate_prob[num_rate];
            if (debug_mode)
                printf("Rate: %dMbps, Prob: %d\n", rate_value[num_rate], rate_prob[num_rate]);
            num_rate++;
        }
    }

    fclose(fd);

    /* By default, fanout size is 1 */
    if (num_fanout == 0)
    {
        num_fanout = 1;
        fanout_size[0] = 1;
        fanout_prob[0] = 100;
        fanout_prob_total = fanout_prob[0];
        if (debug_mode)
            printf("Fanout: %d, Prob: %d\n", fanout_size[0], fanout_prob[0]);
    }

    if (debug_mode)
        printf("Max Fanout: %d\n", max_fanout_size);

    /* By default, DSCP value is 0 */
    if (num_service == 0)
    {
        num_service = 1;
        service_dscp[0] = 0;
        service_prob[0] = 100;
        service_prob_total = service_prob[0];
        if (debug_mode)
            printf("Service DSCP: %d, Prob: %d\n", service_dscp[0], service_prob[0]);
    }

    /* By default, no rate limiting */
    if (num_rate == 0)
    {
        num_rate = 1;
        rate_value[0] = 0;
        rate_prob[0] = 100;
        rate_prob_total = rate_prob[0];
        if (debug_mode)
            printf("Rate: %dMbps, Prob: %d\n", rate_value[0], rate_prob[0]);
    }

    if (load > 0)
    {
        period_us = avg_CDF(req_size_dist) * 8 / load;
        if (period_us <= 0)
        {
            cleanup();
            error("Error: period_us is not positive");
        }
    }
    else
    {
        cleanup();
        error("Error: load is not positive");
    }
}

/* Set request variables */
void set_req_variables()
{
    int i, k, server_id, flow_id = 0;
    unsigned long req_size_total = 0;
    double req_dscp_total = 0;
    unsigned long req_rate_total = 0;
    unsigned long req_interval_total = 0;

    /*per-request variables */
    req_size = (int*)malloc(req_total_num * sizeof(int));
    req_fanout = (int*)malloc(req_total_num * sizeof(int));
    req_server_flow_count = (int**)malloc(req_total_num * sizeof(int*));
    req_dscp = (int*)malloc(req_total_num * sizeof(int));
    req_rate = (int*)malloc(req_total_num * sizeof(int));
    req_sleep_us = (int*)malloc(req_total_num * sizeof(int));
    req_start_time = (struct timeval*)calloc(req_total_num, sizeof(struct timeval));
    req_stop_time = (struct timeval*)calloc(req_total_num, sizeof(struct timeval));

    if (!req_size || !req_fanout || !req_server_flow_count || !req_dscp || !req_rate || !req_sleep_us || !req_start_time || !req_stop_time)
    {
        cleanup();
        error("Error: malloc per-request variables");
    }

    /* Per request */
    for (i = 0; i < req_total_num; i++)
    {
        req_server_flow_count[i] = (int*)calloc(num_server, sizeof(int));   //Initialize as 0
        if (!req_server_flow_count[i])
        {
            cleanup();
            error("Error: malloc per-request variables");
        }

        req_size[i] = gen_random_CDF(req_size_dist);    //request size
        req_fanout[i] = gen_value_weight(fanout_size, fanout_prob, num_fanout, fanout_prob_total);  //request fanout
        req_dscp[i] = gen_value_weight(service_dscp, service_prob, num_service, service_prob_total);    //request DSCP
        req_rate[i] = gen_value_weight(rate_value, rate_prob, num_rate, rate_prob_total);   //sending rate
        req_sleep_us[i] = poission_gen_interval(1.0/period_us); //sleep interval based on poission process

        req_size_total += req_size[i];
        req_dscp_total += req_dscp[i];
        req_rate_total += req_rate[i];
        req_interval_total += req_sleep_us[i];
        flow_total_num += req_fanout[i];

        /* Each flow in this request */
        for (k = 0; k < req_fanout[i]; k++)
        {
            server_id = rand() % num_server;
            req_server_flow_count[i][server_id]++;
            server_flow_count[server_id]++;
        }
    }

    /*per-flow variables */
    flow_req_id = (int*)malloc(flow_total_num * sizeof(int));
    flow_start_time = (struct timeval*)malloc(flow_total_num * sizeof(struct timeval));
    flow_stop_time = (struct timeval*)malloc(flow_total_num * sizeof(struct timeval));

    if (!flow_req_id || !flow_start_time || !flow_stop_time)
    {
        cleanup();
        error("Error: malloc per-flow variables");
    }

    /* Assign request ID to each flow */
    flow_id = 0;
    for (i = 0; i < req_total_num; i++)
        for (k = 0; k < req_fanout[i]; k++)
            flow_req_id[flow_id++] = i;

    if (flow_id != flow_total_num)
        perror("Not all the flows have request ID");

    printf("===============================\n");
    printf("We generate %d requests (%d flows).\n", req_total_num, flow_total_num);

    for (i = 0; i < num_server; i++)
        printf("%s:%d    %d flows\n", server_addr[i], server_port[i], server_flow_count[i]);

    printf("===============================\n");
    printf("The average request arrival interval is %lu us.\n", req_interval_total/req_total_num);
    printf("The average request size is %lu bytes.\n", req_size_total/req_total_num);
    printf("The average flow size is %lu bytes.\n", req_size_total/flow_total_num);
    printf("The average request fanout size is %.2f.\n", (double)flow_total_num/req_total_num);
    printf("The average request DSCP value is %.2f.\n", req_dscp_total/req_total_num);
    printf("The average request sending rate is %lu mbps.\n", req_rate_total/req_total_num);
}

/* Receive traffic from established connections */
void *listen_connection(void *ptr)
{
    struct Conn_Node* node = (struct Conn_Node*)ptr;
    unsigned int meta_data_size = 4 * sizeof(unsigned int);
    unsigned int flow_id, flow_size, flow_tos, flow_rate;   //flow request meta data
    char read_buf[TG_MAX_READ] = {'\0'};

    while (true)
    {
        if (read_exact(node->sockfd, read_buf, meta_data_size, meta_data_size, false) != meta_data_size)
        {
            perror("Error: read meata data");
            break;
        }

        /* extract meta data */
        memcpy(&flow_id, read_buf, sizeof(unsigned int));
        memcpy(&flow_size, read_buf + sizeof(unsigned int), sizeof(unsigned int));
        memcpy(&flow_tos, read_buf + 2 * sizeof(unsigned int), sizeof(unsigned int));
        memcpy(&flow_rate, read_buf + 3 * sizeof(unsigned int), sizeof(unsigned int));

        if (read_exact(node->sockfd, read_buf, flow_size, TG_MAX_READ, true) != flow_size)
        {
            perror("Error: receive flow");
            break;
        }

        node->busy = false;
        pthread_mutex_lock(&(node->list->lock));
        if (flow_id != 0) //Not the special flow ID
        {
            node->list->flow_finished++;
            node->list->available_len++;
        }
        /* Ohterwise, it's a special flow ID to terminate connection.
           So this connection will no longer be available. */
        pthread_mutex_unlock(&(node->list->lock));

        /* A special flow ID to terminate persistent connection */
        if (flow_id == 0)
            break;
        else
        {
            gettimeofday(&flow_stop_time[flow_id - 1], NULL);
            gettimeofday(&req_stop_time[flow_req_id[flow_id - 1]], NULL);
        }
    }

    close(node->sockfd);
    node->connected = false;
    node->busy = false;

    return (void*)0;
}

/* Generate incast requests */
void run_requests()
{
    int i = 0;
    int req_duration_us = 0;
    int sleep_us = 0;
    struct timeval req_tv_start, req_tv_end;

    for (i = 0; i < req_total_num; i++)
    {
        gettimeofday(&req_tv_start, NULL);
        run_request(i);
        gettimeofday(&req_tv_end, NULL);
        req_duration_us = (req_tv_end.tv_sec - req_tv_start.tv_sec) * 1000000 + req_tv_end.tv_usec - req_tv_start.tv_usec;

        sleep_us = sleep_us + req_sleep_us[i];
        if (sleep_us > usleep_overhead_us + req_duration_us)
        {
            usleep(sleep_us - usleep_overhead_us - req_duration_us);
            sleep_us = 0;
        }
    }
}

/* Generate a incast request to some servers */
void run_request(unsigned int req_id)
{
    int conn_id, num_conn, num_conn_new = 0;
    int i, k = 0;
    struct Flow* flow_req = (struct Flow*)malloc(req_fanout[req_id] * sizeof(struct Flow));
    pthread_t *threads = (pthread_t*)malloc(req_fanout[req_id] * sizeof(pthread_t));
    struct Conn_Node** incast_server_conn = NULL;   //per-server incast connections
    struct Conn_Node* tail_node = NULL;

    if (!flow_req || !threads)
    {
        perror("Error: malloc");
        free(flow_req);
        free(threads);
        return;
    }

    conn_id = 0;
    /* Pre-establish all connections of this incast request*/
    for (i = 0; i < num_server; i++)
    {
        num_conn = req_server_flow_count[req_id][i];
        if (num_conn == 0)  //no connection to this server
            continue;

        num_conn_new = max(num_conn - connection_lists[i].available_len, 0);    //the number of new connections we need to establish
        if (num_conn_new > 0)
        {
            tail_node = connection_lists[i].tail;
            /* Establish new connections */
            if (Insert_Conn_List(&connection_lists[i], num_conn_new))
            {
                /* Start listen_connection threads on new established connections */
                while (true)
                {
                    tail_node = tail_node->next;
                    if (tail_node)
                        pthread_create(&(tail_node->thread), NULL, listen_connection, (void*)tail_node);
                    else
                        break;
                }

                if (debug_mode)
                    printf("Establish %d new connections to %s:%d (available/total = %u/%u)\n", num_conn_new, server_addr[i], server_port[i], connection_lists[i].available_len, connection_lists[i].len);
            }
            else
            {
                if (debug_mode)
                    printf("Cannot establish %d new connections to %s:%d (available/total = %u/%u)\n", num_conn_new, server_addr[i], server_port[i], connection_lists[i].available_len, connection_lists[i].len);

                perror("Error: Insert_Conn_List");
                free(flow_req);
                free(threads);
                return;
            }
        }

        incast_server_conn = Search_N_Conn_List(&connection_lists[i], num_conn);
        if (incast_server_conn)
        {
            for (k = 0; k < num_conn; k++)
            {
                flow_req[conn_id].node = incast_server_conn[k];
                flow_req[conn_id].flow_id = global_flow_id + 1; //we reserve flow ID 0 for terminating connections
                flow_req[conn_id].flow_size = req_size[req_id]/req_fanout[req_id];
                flow_req[conn_id].flow_tos = req_dscp[req_id] * 4;  //ToS = 4 * DSCP
                flow_req[conn_id].flow_rate = req_rate[req_id];
                conn_id++;
                global_flow_id++;
            }
            free(incast_server_conn);
        }
        else
        {
            perror("Error: Search_N_Conn_List");
            free(flow_req);
            free(threads);
            return;
        }
    }

    if (conn_id != req_fanout[req_id])
    {
        perror("Error: no enough connections");
        free(flow_req);
        free(threads);
        return;
    }

    gettimeofday(&req_start_time[req_id], NULL);
    /* Generate requests to servers */
    for (i = 0; i < req_fanout[req_id]; i++)
        pthread_create(&threads[i], NULL, run_flow, (void*)(&flow_req[i]));

    for (i = 0; i < req_fanout[req_id]; i++)
        pthread_join(threads[i], NULL);

    free(flow_req);
    free(threads);
}

/* Generate a flow request to a server */
void *run_flow(void *ptr)
{
    int sockfd;
    unsigned int meta_data_size = 4 * sizeof(unsigned int);
    char buf[4 * sizeof(unsigned int)] = {'\0'}; // buffer to hold meta data
    struct Flow f = *(struct Flow*)ptr;
    struct Conn_Node* node = f.node;
    unsigned int flow_id = f.flow_id;
    unsigned int flow_size = f.flow_size;
    unsigned int flow_tos = f.flow_tos;
    unsigned int flow_rate = f.flow_rate;

    /* fill in meta data */
    memcpy(buf, &flow_id, sizeof(unsigned int));
    memcpy(buf + sizeof(unsigned int), &flow_size, sizeof(unsigned int));
    memcpy(buf + 2 * sizeof(unsigned int), &flow_tos, sizeof(unsigned int));
    memcpy(buf + 3 * sizeof(unsigned int), &flow_rate, sizeof(unsigned int));

    /* Send request and record start time */
    if (flow_id)
        gettimeofday(&flow_start_time[flow_id - 1], NULL);

    sockfd = node->sockfd;
    node->busy = true;
    pthread_mutex_lock(&(node->list->lock));
    node->list->available_len--;
    pthread_mutex_unlock(&(node->list->lock));

    if(write_exact(sockfd, buf, meta_data_size, meta_data_size, 0, flow_tos, 0, false) != meta_data_size)
        perror("Error: write meta data");

    return (void*)0;
}

/* Terminate all existing connections */
void exit_connections()
{
    int i = 0;
    struct Conn_Node* ptr = NULL;

    /* Start threads to receive traffic */
    for (i = 0; i < num_server; i++)
    {
        ptr = connection_lists[i].head;
        while (true)
        {
            if (!ptr)
                break;
            else
            {
                if (ptr->connected)
                    exit_connection(ptr);
                ptr = ptr->next;
            }
        }
    }
}

/* Terminate a connection */
void exit_connection(struct Conn_Node* node)
{
    struct Flow f;
    f.node = node;
    f.flow_id = 0;
    f.flow_size = 100;
    f.flow_tos = 0;
    f.flow_rate = 0;

    run_flow((void*)&f);
}

void print_statistic()
{
    unsigned long duration_us = (tv_end.tv_sec - tv_start.tv_sec) * 1000000 + tv_end.tv_usec - tv_start.tv_usec;
    unsigned long req_size_total = 0;
    unsigned long fct_us, rct_us;
    unsigned long goodput_mbps;
    int req_id;
    int i = 0;
    FILE *fd = NULL;

    fd = fopen(rct_log_name, "w");
    if (!fd)
    {
        cleanup();
        error("Error: fopen");
    }

    for (i = 0; i < req_total_num; i++)
    {
        req_size_total += req_size[i];
        rct_us = (req_stop_time[i].tv_sec - req_start_time[i].tv_sec) * 1000000 + req_stop_time[i].tv_usec - req_start_time[i].tv_usec;
        fprintf(fd, "%d %lu %d %d %d\n", req_size[i], rct_us, req_dscp[i], req_fanout[i], req_rate[i]); //size, RCT(us), DSCP, fanout, rate(Mbps)

        if ((req_stop_time[i].tv_sec == 0) && (req_stop_time[i].tv_usec == 0))
            printf("Unfinished request %d\n", i);
    }
    fclose(fd);

    fd = fopen(fct_log_name, "w");
    if (!fd)
    {
        cleanup();
        error("Error: fopen");
    }

    for (i = 0; i < flow_total_num; i++)
    {
        fct_us = (flow_stop_time[i].tv_sec - flow_start_time[i].tv_sec) * 1000000 + flow_stop_time[i].tv_usec - flow_start_time[i].tv_usec;
        req_id = flow_req_id[i];
        fprintf(fd, "%d %lu %d %d\n", req_size[req_id]/req_fanout[req_id], fct_us, req_dscp[req_id], req_rate[req_id]);    //size, FCT(us), DSCP, rate(Mbps)

        if ((flow_stop_time[i].tv_sec == 0) && (flow_stop_time[i].tv_usec == 0))
            printf("Unfinished flow %d\n", i);
    }
    fclose(fd);

    goodput_mbps = req_size_total * 8 / duration_us;
    printf("Achieved goodput is %lu mbps\n", goodput_mbps);
    printf("Write RCT results to %s\n", rct_log_name);
    printf("Write FCT results to %s\n", fct_log_name);
    printf("===============================\n");

    for (i = 0; i < num_server; i++)
        Print_Conn_List(&connection_lists[i]);
}

/* Clean up resources */
void cleanup()
{
    int i = 0;

    free(server_port);
    free(server_addr);
    free(server_flow_count);

    free(fanout_size);
    free(fanout_prob);

    free(service_dscp);
    free(service_prob);

    free(rate_value);
    free(rate_prob);

    free_CDF(req_size_dist);
    free(req_size_dist);

    free(req_size);
    free(req_fanout);
    free(req_dscp);
    free(req_rate);
    free(req_sleep_us);
    free(req_start_time);
    free(req_stop_time);

    if (req_server_flow_count)
    {
        for(i = 0; i < req_total_num; i++)
            free(req_server_flow_count[i]);
    }
    free(req_server_flow_count);

    free(flow_req_id);
    free(flow_start_time);
    free(flow_stop_time);

    if (connection_lists)
    {
        for(i = 0; i < num_server; i++)
            Clear_Conn_List(&connection_lists[i]);
    }
    free(connection_lists);
}