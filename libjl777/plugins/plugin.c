//
//  plugins.c
//  SuperNET API extension
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <ctype.h>
#include "nn.h"
#include "bus.h"
//#include "../includes/pair.h"
//#include "../includes/pubsub.h"
#include "cJSON.c"

double milliseconds(void)
{
    static struct timeval timeval,first_timeval;
    gettimeofday(&timeval,0);
    if ( first_timeval.tv_sec == 0 )
    {
        first_timeval = timeval;
        return(0);
    }
    return((timeval.tv_sec - first_timeval.tv_sec) * 1000. + (timeval.tv_usec - first_timeval.tv_usec)/1000.);
}

void randombytes(uint8_t *x,uint64_t xlen)
{
    static int fd = -1;
    int32_t i;
    if (fd == -1) {
        for (;;) {
            fd = open("/dev/urandom",O_RDONLY);
            if (fd != -1) break;
            sleep(1);
        }
    }
    while (xlen > 0) {
        if (xlen < 1048576) i = (int32_t)xlen; else i = 1048576;
        i = (int32_t)read(fd,x,i);
        if (i < 1) {
            sleep(1);
            continue;
        }
        x += i;
        xlen -= i;
    }
}

int32_t mygetline(char *line,int32_t max)
{
    struct timeval timeout;
    fd_set fdset;
    int32_t s;
    line[0] = 0;
    FD_ZERO(&fdset);
    FD_SET(STDIN_FILENO,&fdset);
    timeout.tv_sec = 0, timeout.tv_usec = 100000;
    if ( (s= select(1,&fdset,NULL,NULL,&timeout)) < 0 )
        fprintf(stderr,"wait_for_input: error select s.%d\n",s);
    else if ( FD_ISSET(STDIN_FILENO,&fdset) == 0 || fgets(line,max,stdin) != 0 )
        return(-1);//sprintf(retbuf,"{\"result\":\"no messages\",\"myid\":\"%llu\",\"counter\":%d}",(long long)myid,counter), retbuf[0] = 0;
    return(strlen(line));
}

int32_t get_socket_status(int32_t sock,int32_t timeoutmillis)
{
    struct nn_pollfd pfd;
    int32_t rc;
    pfd.fd = sock;
    pfd.events = NN_POLLIN | NN_POLLOUT;
    if ( (rc= nn_poll(&pfd,1,timeoutmillis)) == 0 )
        return(pfd.revents);
    else return(-1);
}

int32_t get_newinput(int32_t permanentflag,char *line,int32_t max,int32_t sock,int32_t timeoutmillis)
{
    int32_t rc,len;
    char *jsonstr = 0;
    line[0] = 0;
    if ( (permanentflag != 0 || ((rc= get_socket_status(sock,timeoutmillis)) > 0 && (rc & NN_POLLIN) != 0)) && (len= nn_recv(sock,&jsonstr,NN_MSG,0)) > 0 )
    {
        strncpy(line,jsonstr,max-1);
        line[max-1] = 0;
        nn_freemsg(jsonstr);
    }
    else if ( permanentflag == 0 )
        mygetline(line,max);
    return((int32_t)strlen(line));
}

int32_t init_daemonsock(int32_t permanentflag,uint64_t myid,uint64_t daemonid,int32_t timeoutmillis)
{
    int32_t sock,err;
    char addr[64];
    if ( permanentflag == 0 )
    {
        if ( (sock= nn_socket(AF_SP,NN_BUS)) < 0 )
        {
            printf("error %d nn_socket err.%s\n",sock,nn_strerror(nn_errno()));
            return(-1);
        }
        sprintf(addr,"ipc://%llu",(long long)daemonid);
        if ( (err= nn_connect(sock,addr)) < 0 )
        {
            printf("error %d nn_connect err.%s (%llu to %s)\n",sock,nn_strerror(nn_errno()),(long long)daemonid,addr);
            return(-1);
        }
    }
    else
    {
        sprintf(addr,"ipc://%llu",(long long)daemonid+1);
        if ( (sock= nn_socket(AF_SP,NN_BUS)) < 0 )
        {
            printf("error %d nn_socket err.%s\n",sock,nn_strerror(nn_errno()));
            return(-1);
        }
        if ( 1 && (err= nn_bind(sock,addr)) < 0 )
        {
            printf("error %d nn_bind.%d (%s) | %s\n",err,sock,addr,nn_strerror(nn_errno()));
            return(-1);
        }
        sprintf(addr,"ipc://%llu",(long long)daemonid+2);
        if ( 0 && (err= nn_connect(sock,addr)) < 0 )
        {
            printf("error %d nn_connect err.%s (%llu to %s)\n",sock,nn_strerror(nn_errno()),(long long)daemonid,addr);
            return(-1);
        }
    }
    nn_setsockopt(sock,NN_SOL_SOCKET,NN_RCVTIMEO,&timeoutmillis,sizeof(timeoutmillis));
    printf("daemonsock: %d nn_connect (%llu <-> %s)\n",sock,(long long)daemonid,addr);
    return(sock);
}

void process_daemon_json(char *retbuf,char *jsonstr,cJSON *json)
{
    char *str;
    str = stringifyM(jsonstr);
    sprintf(retbuf,"{\"args\":%s,\"milliseconds\":%f}\n",str,milliseconds());
    free(str);
}

int32_t process_plugin_json(int32_t permanentflag,uint64_t daemonid,int32_t sock,uint64_t myid,char *retbuf,long max,char *jsonstr)
{
    int32_t err,i,n,len = (int32_t)strlen(jsonstr);
    cJSON *json,*array;
    uint64_t instanceid,sender;
    char addr[64];
    retbuf[0] = 0;
    if ( (json= cJSON_Parse(jsonstr)) != 0 )
    {
        if ( (sender= get_API_nxt64bits(cJSON_GetObjectItem(json,"myid"))) != myid )
        {
            if ( sender == daemonid )
                process_daemon_json(retbuf,jsonstr,json);
            else if ( sender != 0 ) printf("process message from %llu: (%s)\n",(long long)sender,jsonstr), fflush(stdout);
        } else printf("gotack.(%s) %f\n",jsonstr,milliseconds()), fflush(stdout);
    }
    else
    {
        if ( jsonstr[len-1] == '\r' || jsonstr[len-1] == '\n' || jsonstr[len-1] == '\t' || jsonstr[len-1] == ' ' )
            jsonstr[--len] = 0;
        if ( strcmp(jsonstr,"getpeers") == 0 )
            sprintf(retbuf,"{\"pluginrequest\":\"SuperNET\",\"requestType\":\"getpeers\"}");
        else sprintf(retbuf,"{\"result\":\"echo\",\"permanent\":%d,\"myid\":\"%llu\",\"message\":\"%s\",\"millis\":%f}",permanentflag,(long long)myid,jsonstr,milliseconds());
    }
    return(strlen(retbuf));
}

void process_json(char *jsonargs)
{
    char line[8192],retbuf[8192],*retstr,*jsonstr;
    cJSON *json;
    json = cJSON_Parse(jsonargs);
    jsonstr = cJSON_Print(json);
    //fprintf(stderr,"got jsonargs.(%s) %s %p\n",jsonargs,jsonstr,json);
    process_daemon_json(line,jsonstr,json);
    if ( jsonstr != 0 )
        free(jsonstr);
    if ( json != 0 )
        free_json(json);
    printf("%s\n",line), fflush(stdout);
}

int main(int argc,const char *argv[])
{
    uint64_t daemonid,myid;
    int32_t permanentflag,rc,sock,len,timeout,ppid,counter = 0;
    char line[8192],retbuf[8192],*retstr,*jsonargs,*jsonstr;
    milliseconds();
    ppid = getppid();
    fprintf(stderr,"(%s).argc%d parent PID.%d\n",argv[0],argc,ppid);
    timeout = 1;
    if ( argc <= 2 )
    {
        jsonargs = (argc > 1) ? (char *)argv[1]:"{}";
        process_json(jsonargs);
        //fprintf(stderr,"PLUGIN_RETURNS.[%s]\n",line), fflush(stdout);
        return(0);
    }
    randombytes((uint8_t *)&myid,sizeof(myid));
    permanentflag = atoi(argv[1]);
    daemonid = atol(argv[2]);
    if ( argc >= 3 )
        process_json((char *)argv[3]);
    fprintf(stderr,"argc.%d: myid.%llu daemonid.%llu args.(%s)\n",argc,(long long)myid,(long long)daemonid,argc>=3?argv[3]:"");
    if ( (sock= init_daemonsock(permanentflag,myid,daemonid,timeout)) >= 0 )
    {
        while ( ppid == getppid() )
        {
            if ( (len= get_newinput(permanentflag,line,sizeof(line),sock,timeout)) > 0 )
            {
                if ( line[len-1] == '\n' )
                    line[--len] = 0;
                counter++;
                printf("%d <<<<<<<<<<<<<< RECEIVED (%s).%d -> daemonid.%llu PERM.%d\n",counter,line,len,(long long)daemonid,permanentflag), fflush(stdout);
                if ( (len= process_plugin_json(permanentflag,daemonid,sock,myid,retbuf,sizeof(retbuf),line)) > 1 )
                {
                    printf("%s\n",retbuf), fflush(stdout);
                    nn_send(sock,retbuf,len+1,0); // send the null terminator too
                }
            }
            usleep(1000);
        }
        nn_shutdown(sock,0);
    }
    printf("plugin.(%s) exiting\n",argv[0]), fflush(stdout);
    return(0);
}