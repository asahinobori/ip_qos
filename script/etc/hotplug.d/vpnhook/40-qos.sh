#!/bin/sh

. /lib/functions.sh

_qos_delete_rule() {
    local section=${1}
    config_get valx ${section} if_pong

    for element in $interfaces; do
        [ "${element}" = "${valx}" ] && {
            uci delete qos.${section}
        }
    done
}

_qos_delete_iface() {
    local section=${1}
    config_get valx ${section} t_name

    for element in $interfaces; do
        [ "${element}" = "${valx}" ] && {
            uci delete qos.${section}
            uci del_list qos.setting.interface=${element}

        }
    done
}

case ${ACTION} in
    DELETE)
        [ -n "${interfaces}" ] && {
            echo "interfaces=$interfaces" >> /tmp/ip_qos/vpnhook.log
            interfaces=${interfaces//,/ }
            config_load qos
            config_foreach _qos_delete_rule rule
            config_foreach _qos_delete_iface interface
            uci_commit qos

            #/etc/init.d/qos-tplink start
        }
    ;;
    ADD)
    ;;
    *)
    ;;
esac