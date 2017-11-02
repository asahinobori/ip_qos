#!/bin/sh

# dispatch operation and construct relative command

local n=$#
[ "$n" -eq 0 ] && return

. /lib/ip_qos/public.sh

local nn="$@"
QOS_WRITE_LOG " $0 qos_dispatch ${nn} begin !"

# Deal with qos polling daemon process
[ $QOS_THRESHOLD -gt 0 ] && {
    [ $1 = "stop" ] && {
        [ -f $QOS_POLLING_FILE ] && {
            kill -9 `cat $QOS_POLLING_FILE`
            rm -f $QOS_POLLING_FILE 2>/dev/null
            QOS_WRITE_LOG " stop qos polling"
        }
    }

    [ $1 = "start" ] && {
        # if polling exist, kill the old polling process and create new
        [ -f $QOS_POLLING_FILE ] && {
            kill -9 `cat $QOS_POLLING_FILE`
            rm -f $QOS_POLLING_FILE 2>/dev/null
            QOS_WRITE_LOG " stop qos polling"
        }
        /lib/ip_qos/polling.sh &
        touch $QOS_POLLING_FILE
        echo `ps -w | grep -w polling.sh | awk '/\{polling\}/ {print $1}'` > $QOS_POLLING_FILE
        QOS_WRITE_LOG " start qos polling"
    }

    [ ! -f $QOS_READY ] && {
        [ $1 != "stop" ] && {
            # throughout not enough, keep polling and do nothing
            QOS_WRITE_LOG " info, polling daemon show that throughout lower than $QOS_THRESHOLD% now, just return the $1 action "
            return
        }
    }
}

[ $QOS_THRESHOLD -eq 0 ] && {
    [ -f $QOS_POLLING_FILE ] && {
        kill -9 `cat $QOS_POLLING_FILE`
        rm -f $QOS_POLLING_FILE 2>/dev/null
        QOS_WRITE_LOG " stop qos polling"
    }
}

# compose the usr_cmd
usr_cmd=". /lib/ip_qos/main.sh"

for i in $(seq 1 1 $n); do
    usr_cmd="$usr_cmd $1"
    shift
done

$usr_cmd
return
