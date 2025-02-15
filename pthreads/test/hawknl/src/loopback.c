/*
  HawkNL cross platform network library
  Copyright (C) 2000-2002 Phil Frisbie, Jr. (phil@hawksoft.com)

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the
  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
  Boston, MA  02111-1307, USA.

  Or go to http://www.gnu.org/copyleft/lgpl.html
*/


#include <string.h>
#include <stdio.h>

#if defined (_WIN32_WCE)
#define errno GetLastError()
#else
#include <errno.h>
#endif

#include "nlinternal.h"

#ifdef WINDOWS_APP
/* Windows systems */
#ifdef _MSC_VER
#pragma warning (disable:4201)
#pragma warning (disable:4214)
#pragma warning (disable:4115)
#pragma warning (disable:4514)
#pragma warning (disable:4127)
#endif /* _MSC_VER */

#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#ifdef _MSC_VER
#pragma warning (default:4201)
#pragma warning (default:4214)
#pragma warning (default:4115)
#endif /* _MSC_VER */

#else
/* Unix-style systems or macs with posix support */
#include <netinet/in.h> /* for ntohs and htons */
#endif

#ifdef _MSC_VER
#pragma warning (disable:4100) /* disable "unreferenced formal parameter" */
#endif

#ifdef NL_INCLUDE_LOOPBACK
#include "loopback.h"

static NLaddress loopback_ouraddress;
static NLint loopgroup;
static volatile NLboolean reuseaddress = NL_FALSE;

static NLmutex  portlock; /* In memory of my step-father, Don Portlock,
                                       who passed away Jan 12, 2001 - Phil */

static volatile NLushort newport = 1024;

static NLushort loopback_getNextPort(void)
{
    nlMutexLock(&portlock);
    if(++newport > 65535)
    {
        newport = 1024;
    }
    nlMutexUnlock(&portlock);
    return newport;
}

static NLboolean loopback_ScanPort(NLushort port, NLenum type)
{
    NLint           numsockets = NL_MAX_GROUP_SOCKETS;
    NLsocket        temp[NL_MAX_GROUP_SOCKETS];

    if(reuseaddress == NL_TRUE)
    {
        return NL_TRUE;
    }
    if(nlGroupGetSockets(loopgroup, (NLsocket *)&temp, &numsockets) == NL_FALSE)
    {
        return NL_FALSE;
    }
    if(numsockets == 0)
    {
        return NL_TRUE;
    }
    while(numsockets-- > 0)
    {
        nl_socket_t *sock = nlSockets[temp[numsockets]];

        if(sock->type == type && sock->localport == port)
        {
            return NL_FALSE;
        }
    }
    return NL_TRUE;
}

static NLushort loopback_TryPort(NLushort port, NLenum type)
{
    NLint   ntries = 500; /* this is to prevent an infinite loop */

    if(port > 0)
    {
        if(loopback_ScanPort(port, type) == NL_TRUE)
        {
            return port;
        }
        else
        {
            return 0;
        }
    }
    /* let's find our own port number */
    while(ntries-- > 0)
    {
        port = loopback_getNextPort();
        if(loopback_ScanPort(port, type) == NL_TRUE)
        {
            return port;
        }
    }
    return 0;
}

NLboolean loopback_Init(void)
{
    loopgroup = nlGroupCreate();

    if(loopgroup == NL_INVALID)
    {
        return NL_FALSE;
    }
    return NL_TRUE;
}

void loopback_Shutdown(void)
{
    nlGroupDestroy(loopgroup);
}

NLboolean loopback_Listen(NLsocket socket)
{
    nl_socket_t *sock = nlSockets[socket];
    NLint       i = NL_MAX_ACCEPT;

    if(sock->type == NL_BROADCAST)
    {
        nlSetError(NL_WRONG_TYPE);
        return NL_FALSE;
    }
    sock->listen = NL_TRUE;
    while(i-- > 0)
    {
        sock->loop->accept[i] = NL_INVALID;
    }
    return NL_TRUE;
}

NLsocket loopback_AcceptConnection(NLsocket socket)
{
    nl_socket_t *sock = nlSockets[socket];
    NLint       i;

    if(sock->listen == NL_FALSE)
    {
        nlSetError(NL_NOT_LISTEN);
        return NL_INVALID;
    }
    /* look for a valid socket */
    for(i=0;i<NL_MAX_ACCEPT;i++)
    {
        if(sock->loop->accept[i] != NL_INVALID)
        {
            NLsocket    newsocket = loopback_Open(0, sock->type);
            NLsocket    osock = sock->loop->accept[i];
            nl_socket_t *othersock = nlSockets[osock];

            if(newsocket != NL_INVALID)
            {
                nl_socket_t *newsock = nlSockets[newsocket];

                nlLockSocket(osock, NL_BOTH);
                /* do the connecting */
                newsock->loop->consock = osock;
                newsock->remoteport = othersock->localport;
                othersock->loop->consock = newsocket;
                othersock->remoteport = newsock->localport;
                newsock->connected = NL_TRUE;
                othersock->connected = NL_TRUE;
                othersock->connecting = NL_FALSE;
                loopback_SetAddrPort(&othersock->address, othersock->remoteport);
                loopback_SetAddrPort(&newsock->address, newsock->remoteport);
                sock->loop->accept[i] = NL_INVALID;
                nlUnlockSocket(osock, NL_BOTH);
                return newsocket;
            }
        }
    }
    nlSetError(NL_NO_PENDING);
    return NL_INVALID;
}

NLsocket loopback_Open(NLushort port, NLenum type)
{
    nl_socket_t *newsock;
    NLsocket    newsocket;
    NLint       i;
    NLushort    lport;

    switch (type) {

    case NL_RELIABLE:
    case NL_UNRELIABLE:
    case NL_RELIABLE_PACKETS:
    case NL_BROADCAST:
        break;

    default:
        nlSetError(NL_INVALID_ENUM);
        return NL_INVALID;
    }

    lport = loopback_TryPort(port, type);
    if(lport == 0)
    {
        nlSetError(NL_INVALID_PORT);
        return NL_INVALID;
    }
    newsocket = nlGetNewSocket();
    if(newsocket == NL_INVALID)
    {
        return NL_INVALID;
    }
    newsock = nlSockets[newsocket];
    newsock->type = type;
    newsock->localport = lport;
    if(type == NL_BROADCAST)
    {
        newsock->remoteport = lport;
    }

    if((newsock->loop = (nl_loop_t *)malloc(sizeof(nl_loop_t))) == NULL)
    {
        nlSetError(NL_OUT_OF_MEMORY);
        nlUnlockSocket(newsocket, NL_BOTH);
        loopback_Close(newsocket);
        return NL_INVALID;
    }
    /* clear out the structure */
    memset(newsock->loop, 0, sizeof(nl_loop_t));
    for(i=0;i<NL_NUM_PACKETS;i++)
    {
        NLboolean err = NL_FALSE;

        /* malloc the max packet length plus two bytes for the port number */
        if((newsock->loop->inpacket[i] = (NLbyte *)malloc((size_t)(NL_MAX_PACKET_LENGTH + 2))) == NULL)
        {
            nlSetError(NL_OUT_OF_MEMORY);
            err = NL_TRUE;
        }
        if(err == NL_TRUE)
        {
            while(i-- > 0)
            {
                free(newsock->loop->inpacket[i]);
            }
            nlUnlockSocket(newsocket, NL_BOTH);
            loopback_Close(newsocket);
            return NL_INVALID;
        }
    }

    (void)nlGroupAddSocket(loopgroup, newsocket);
    nlUnlockSocket(newsocket, NL_BOTH);

    return newsocket;
}

NLboolean loopback_Connect(NLsocket socket, const NLaddress *address)
{
    nl_socket_t *sock = nlSockets[socket];
    NLushort    port;
    NLint       numsockets = NL_MAX_GROUP_SOCKETS;
    NLsocket    temp[NL_MAX_GROUP_SOCKETS];

    /* no need to connect a broadcast socket */
    if(sock->type == NL_BROADCAST)
    {
        nlSetError(NL_WRONG_TYPE);
    }
    port = loopback_GetPortFromAddr(address);
    /* make sure socket is not already connected */
    if(sock->connected == NL_TRUE || sock->connecting == NL_TRUE)
    {
        nlSetError(NL_CON_REFUSED);
        return NL_FALSE;
    }
    if(nlGroupGetSockets(loopgroup, (NLsocket *)&temp, &numsockets) == NL_FALSE)
    {
        return NL_FALSE;
    }
    if(numsockets == 0)
    {
        return NL_FALSE;
    }
    while(numsockets-- > 0)
    {
        NLsocket    s = temp[numsockets];
        nl_socket_t *othersock = nlSockets[s];

        if(sock->type == othersock->type && port == othersock->localport
            && othersock->listen == NL_TRUE && othersock->connected == NL_FALSE
            && othersock->connecting == NL_FALSE)
        {
            /* we found the right socket, so connect */
            NLint i;

            nlLockSocket(s, NL_BOTH);
            for(i=0;i<NL_MAX_ACCEPT;i++)
            {
                if(othersock->loop->accept[i] == NL_INVALID)
                {
                    othersock->loop->accept[i] = socket;
                    sock->connecting = NL_TRUE;
                    sock->loop->consock = s;
                    nlUnlockSocket(s, NL_BOTH);
                    return NL_TRUE;
                }
            }
            nlUnlockSocket(s, NL_BOTH);
        }
    }
    nlSetError(NL_CON_REFUSED);
    return NL_FALSE;
}

void loopback_Close(NLsocket socket)
{
    nl_socket_t *sock = nlSockets[socket];
    int         i;

    if(sock->connected == NL_TRUE || sock->connecting == NL_TRUE)
    {
        /* break the connection */
        nl_socket_t *othersock = nlSockets[sock->loop->consock];

        othersock->loop->consock = NL_INVALID;
    }
    for(i=0;i<NL_NUM_PACKETS;i++)
    {
        void /*@owned@*/*t = sock->loop->inpacket[i];

        free(t);
    }
    free(sock->loop);
}

NLint loopback_Read(NLsocket socket, NLvoid *buffer, NLint nbytes)
{
    nl_socket_t *sock = nlSockets[socket];
    NLint       len = sock->loop->inlen[sock->loop->nextinused];
    NLint       c = 0;
    NLushort    port;

    if(len > 0)
    {
        if(len > nbytes)
        {
            nlSetError(NL_BUFFER_SIZE);
            return NL_INVALID;
        }
        if(sock->connecting == NL_TRUE)
        {
            nlSetError(NL_CON_PENDING);
            return NL_INVALID;
        }
        /* get the port number */
        readShort(sock->loop->inpacket[sock->loop->nextinused], c, port);
        loopback_SetAddrPort(&sock->address, port);
        /* copy the packet */
        memcpy(buffer, sock->loop->inpacket[sock->loop->nextinused] + 2, (size_t)len);
        /* zero out length and set up for next packet */
        sock->loop->inlen[sock->loop->nextinused] = 0;
        sock->loop->nextinused++;
        if(sock->loop->nextinused >= NL_NUM_PACKETS)
        {
            sock->loop->nextinused = 0;
        }
    }
    /* check for broken connection */
    if(sock->connected == NL_TRUE && sock->loop->consock == NL_INVALID)
    {
        nlSetError(NL_CON_TERM);
        return NL_INVALID;
    }
    return len;
}

static NLint loopback_WritePacket(NLsocket to, const NLvoid *buffer, NLint nbytes, NLushort fromport)
{
    nl_socket_t *sock = nlSockets[to];
    NLint       i, j;
    NLint       c = 0;

    /* check the packet size */
    if(nbytes > NL_MAX_PACKET_LENGTH)
    {
        nlSetError(NL_PACKET_SIZE);
        return NL_INVALID;
    }
    nlLockSocket(to, NL_READ);
    /* make sure we have an empty packet buffer */
    if(sock->loop->nextinfree == NL_INVALID)
    {
        /* all buffers were filled by last write */
        /* check to see if any were emptied by a read */
        i = NL_NUM_PACKETS;
        j = sock->loop->nextinused;

        while(i-- > 0)
        {
            if(sock->loop->inlen[j] == 0)
            {
                /* found the first free */
                sock->loop->nextinfree = j;
                break;
            }
            j++;
            if(j >= NL_NUM_PACKETS)
            {
                j = 0;
            }
        }
        if(sock->loop->nextinfree == NL_INVALID)
        {
            nlUnlockSocket(to, NL_READ);
            /* none are free */
            if(sock->type == NL_RELIABLE || sock->type == NL_RELIABLE_PACKETS)
            {
                return 0;
            }
            else
            {
                /* silently fail */
                return nbytes;
            }
        }
    }
    /* write the port number */
    writeShort(sock->loop->inpacket[sock->loop->nextinfree], c, fromport);
    /* copy the packet buffer */
    memcpy(sock->loop->inpacket[sock->loop->nextinfree] + 2, buffer, (size_t)nbytes);
    sock->loop->inlen[sock->loop->nextinfree] = nbytes;
    sock->loop->nextinfree++;
    if(sock->loop->nextinfree >= NL_NUM_PACKETS)
    {
        sock->loop->nextinfree = 0;
    }
    /* check for full packet buffers */
    if(sock->loop->inlen[sock->loop->nextinfree] != 0)
    {
        sock->loop->nextinfree = NL_INVALID;
    }
    nlUnlockSocket(to, NL_READ);
    return nbytes;
}

NLint loopback_Write(NLsocket socket, const NLvoid *buffer, NLint nbytes)
{
    nl_socket_t *sock = nlSockets[socket];
    nl_socket_t *othersock;
    NLsocket    s[NL_MAX_GROUP_SOCKETS];
    NLint       number = NL_MAX_GROUP_SOCKETS;
    NLint       i;
    NLint       count;

    switch (sock->type) {

    case NL_RELIABLE:
    case NL_UNRELIABLE:
    case NL_RELIABLE_PACKETS:
    default:
        {
            if(sock->connected == NL_TRUE)
            {
                /* check for broken connection */
                if(sock->loop->consock == NL_INVALID)
                {
                    nlSetError(NL_CON_TERM);
                    return NL_INVALID;
                }
                count = loopback_WritePacket(sock->loop->consock, buffer, nbytes, sock->localport);
            }
            else if(sock->connecting == NL_TRUE)
            {
                nlSetError(NL_CON_PENDING);
                return NL_INVALID;
            }
            count = nbytes;
            (void)nlGroupGetSockets(loopgroup, (NLsocket *)s, &number);
            for(i=0;i<number;i++)
            {
                if(nlIsValidSocket(s[i]) == NL_TRUE)
                {
                    othersock = nlSockets[s[i]];

                    if(sock->remoteport == othersock->localport &&
                        othersock->connected == NL_FALSE &&
                        sock->type == othersock->type)
                    {
                        (void)loopback_WritePacket(s[i], buffer, nbytes, sock->localport);
                    }
                }
            }
        }
        break;
    case NL_BROADCAST:
        {
            count = nbytes;
            (void)nlGroupGetSockets(loopgroup, (NLsocket *)s, &number);
            for(i=0;i<number;i++)
            {
                if(nlIsValidSocket(s[i]) == NL_TRUE)
                {
                    othersock = nlSockets[s[i]];

                    if(sock->localport == othersock->localport &&
                        sock->type == othersock->type)
                    {
                        (void)loopback_WritePacket(s[i], buffer, nbytes, sock->localport);
                    }
                }
            }
        }
    }

    return count;
}

NLchar *loopback_AddrToString(const NLaddress *address, NLchar *string)
{
    _stprintf(string, (const NLchar *)TEXT("127.0.0.1:%u"), loopback_GetPortFromAddr(address));

    return string;
}

NLboolean loopback_StringToAddr(const NLchar *string, NLaddress *address)
{
    NLchar      *st;
    NLint       port;

    memset(address, 0, sizeof(NLaddress));
    address->valid = NL_TRUE;
    /* check for a port number */
    st = _tcschr(string, TEXT(':'));
    if(st != NULL)
    {
        st++;
        port = _ttoi(st);
        if(port < 0)
        {
            port = 0;
        }
        else if(port > 65535)
        {
            port = 0;
        }
        loopback_SetAddrPort(address, (NLushort)port);
    }
    return NL_TRUE;
}

NLboolean loopback_GetLocalAddr(/*@unused@*/ NLsocket socket, NLaddress *address)
{
    memset(address, 0, sizeof(NLaddress));
    address->valid = NL_TRUE;
    return NL_TRUE;
}

NLaddress *loopback_GetAllLocalAddr(NLint *count)
{
    *count = 1;
    memset(&loopback_ouraddress, 0, sizeof(NLaddress));
    loopback_ouraddress.valid = NL_TRUE;
    return &loopback_ouraddress;
}

NLboolean loopback_SetLocalAddr(/*@unused@*/ const NLaddress *address)
{
    return NL_TRUE;
}

NLchar *loopback_GetNameFromAddr(const NLaddress *address, NLchar *name)
{
    _stprintf(name, (const NLchar *)TEXT("%s:%u"), TEXT("localhost"), loopback_GetPortFromAddr(address));
    return name;
}

NLboolean loopback_GetNameFromAddrAsync(const NLaddress *address, NLchar *name)
{
    (void)loopback_GetNameFromAddr(address, name);
    return NL_TRUE;
}

NLboolean loopback_GetAddrFromName(const NLchar *name, NLaddress *address)
{
    loopback_StringToAddr(name, address);
    address->valid = NL_TRUE;
    return NL_TRUE;
}

NLboolean loopback_GetAddrFromNameAsync(const NLchar *name, NLaddress *address)
{
    return loopback_GetAddrFromName(name, address);
}

NLboolean loopback_AddrCompare(const NLaddress *address1, const NLaddress *address2)
{
    if(*(NLushort *)(&address1->sa_data[0]) == *(NLushort *)(&address2->sa_data[0]))
    {
        return NL_TRUE;
    }
    return NL_FALSE;
}

NLushort loopback_GetPortFromAddr(const NLaddress *address)
{
    return *(NLushort *)(&address->sa_data[0]);
}

void loopback_SetAddrPort(NLaddress *address, NLushort port)
{
    *(NLushort *)(&address->sa_data[0]) = port;
}

NLint loopback_GetSystemError(void)
{
    return errno;
}

NLint loopback_PollGroup(NLint group, NLenum name, NLsocket *sockets, NLint number, NLint timeout)
{
    NLint           count = 0;
    NLint           numsockets = NL_MAX_GROUP_SOCKETS;
    NLsocket        temp[NL_MAX_GROUP_SOCKETS];

    if(nlGroupGetSockets(group, (NLsocket *)&temp, &numsockets) == NL_FALSE)
    {
        /* any error is set by nlGroupGetSockets */
        return NL_INVALID;
    }
    if(numsockets == 0)
    {
        return 0;
    }

    switch(name) {

    case NL_READ_STATUS:
        {
            NLint   i = 0;

            while(numsockets-- > 0)
            {
                /* check for a packet */
                nl_socket_t *sock;

                if(nlIsValidSocket(temp[i]) != NL_TRUE)
                {
                    nlSetError(NL_INVALID_SOCKET);
                    return NL_INVALID;
                }
                sock = nlSockets[temp[i]];

                if(sock->loop->inlen[sock->loop->nextinused] > 0)
                {
                    *sockets = temp[i];
                    sockets++;
                    count++;
                    if(count > number)
                    {
                        nlSetError(NL_BUFFER_SIZE);
                        return NL_INVALID;
                    }
                }
                i++;
            }
        }
        break;

    case NL_WRITE_STATUS:
        {
            NLint   i = 0;

            while(numsockets-- > 0)
            {
                nl_socket_t *sock;

                if(nlIsValidSocket(temp[i]) != NL_TRUE)
                {
                    nlSetError(NL_INVALID_SOCKET);
                    return NL_INVALID;
                }
                sock = nlSockets[temp[i]];

                /* check for a free packet if reliable and connected */
                if((sock->type == NL_RELIABLE || sock->type == NL_RELIABLE_PACKETS)
                    && (sock->connecting == NL_TRUE || sock->connected == NL_TRUE))
                {
                    nl_socket_t *othersock = nlSockets[sock->loop->consock];

                    if(othersock->loop->nextinfree == NL_INVALID)
                    {
                        continue;
                    }
                }
                /* add the socket to the list */
                *sockets = temp[i];
                sockets++;
                count++;
                if(count > number)
                {
                    nlSetError(NL_BUFFER_SIZE);
                    return NL_INVALID;
                }
                i++;
            }
        }
        break;

    default:
        nlSetError(NL_INVALID_ENUM);
        return NL_INVALID;
    }
    return count;
}

NLboolean loopback_Hint(NLenum name, NLint arg)
{
    switch (name) {

    case NL_REUSE_ADDRESS:
        reuseaddress = (NLboolean)(arg != 0 ? NL_TRUE : NL_FALSE);
        break;

    default:
        nlSetError(NL_INVALID_ENUM);
        return NL_FALSE;
    }
    return NL_TRUE;
}

#endif /* NL_INCLUDE_LOOPBACK */

