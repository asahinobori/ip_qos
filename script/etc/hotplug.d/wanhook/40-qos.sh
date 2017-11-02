#!/bin/sh

. /lib/functions.sh

mkdir -p "/tmp/ip_qos"
local WANHOOK_LOG="/tmp/ip_qos/wanhook.log"

_qos_delete_rule() {
    local section=${1}
    config_get valx ${section} if_pong

    for element in $interfaces; do
        [ "${element}" = "${valx}" ] && {
            uci delete qos.${section}
        }
    done
}

case ${ACTION} in
    DELETE)
        [ -n "${interfaces}" ] && {
            echo "`date +%Y%m%d-%H:%M:%S` interfaces=$interfaces" >> $WANHOOK_LOG
            interfaces=${interfaces//,/ }
            config_load qos
            config_foreach _qos_delete_rule rule
            uci_commit qos

            #/etc/init.d/qos-tplink start
        }
    ;;
    ADD)
    ;;
    WANMOD)
        # /etc/init.d/qos-tplink start
    ;;
    *)
    ;;
esac