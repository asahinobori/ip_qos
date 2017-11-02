#!/bin/sh

# include dependence
. /lib/ip_qos/public.sh
. /lib/ip_qos/core.sh

local RULE_OPT_LIST="enable if_ping if_pong rate_min rate_max rate_min_mate rate_max_mate ip_group mode time"

qos_destroy()
{
    local destroy_param="$@"

    rm -rf $QOS_READY 2>/dev/null
    QOS_STATE clear

    # delete time object
    ret=$(timeobj_api_deleteAll ${THIS_MODULE})
    ret=$(timeobj_api_commit ${THIS_MODULE})

    # flush netfilter(iptables) qos chains
    qos_nf_destroy $destroy_param

    qmark -r 1

    # flush tc rule tree
    qos_tc_destroy

    qmark -x

    rm -f ${QOS_STOP_FLAG} 2>/dev/null

    QOS_WRITE_LOG " qos_destroy."
}

# get rule_list with the callback $1, which define the match method
qos_rulelist_get()
{
    local cb="$1"
    local list=""
    config_foreach $cb $STYPE_RULE
    QOS_RET "$list"
}

qos_rules_parse()
{
    local rule_list="$1"
    local flag=0  # if have time obj registered
    local rule_num=$(QOS_STATE get rule_num)

    for rule_name in $rule_list; do
        [ -f ${QOS_STOP_FLAG} ] && break

        QOS_WRITE_LOG " qos_rules_parse, parsing rule $rule_name ..."

        [ "$rule_num" -ge "$QOS_RULE_SPEC" ] && {
            QOS_WRITE_LOG " qos_rules_parse, user rule num exceed the spec: $QOS_RULE_SPEC."
            return 1
        }
        let rule_num++

        # check rule should be to install
        # if rule is enable ?
        config_get enable "$rule_name" "enable"
        [ "$enable" != "on" ] && {
            QOS_WRITE_LOG " qos_rules_parse, rule disable, skip install."
            continue
        }

        # if rule has option time, register rule to time manager module, then parse next rule
        config_get timeobj "$rule_name" "time"
        [ "$timeobj" = "" ] && timeobj="Any"
        [ "$timeobj" != "Any" ] && {
            QOS_WRITE_LOG " qos_rules_parse, rule register to tmngtd, timeobj=$timeobj, rule=$rule_name."
            [ "$timeobj" = "SELF_DEFINE" ] && {
                # self define timeobj name is the same as rule name
                timeobj=${rule_name}
            }
            ret=$(timeobj_api_add "$timeobj" $THIS_MODULE $rule_name)
            flag=$((flag + 1))
            continue
        }

        qos_rule_install $rule_name || {
            QOS_WRITE_LOG " qos_rules_parse, qos_rule_parse, install rule $rule_name fail."
            break
        }
    done
    QOS_STATE set rule_num "$rule_num"
    [ $flag -gt 0 ] && ret=$(timeobj_api_commit $THIS_MODULE)

    return 0
}

qos_start()
{
    start_param="$@"

    [ -z $QOS_CONFIG_INVALID ] || {
        QOS_WRITE_LOG " qos_start, no UCI config, exit."
        return
    }

    [ $(qos_global_init) -eq 0 ] && {

        # extract all user rules from UCI config file
        rule_get()
        {
            [ -z "$list" ] && list="$1" || list="$list $1"
        }
        local rule_list=$(qos_rulelist_get rule_get)
        if [ ${#rule_list} -eq 0 ]; then
            QOS_WRITE_LOG " qos_start, no user rule find."
        else
            QOS_WRITE_LOG " qos_start, rule list: $rule_list"
            # store all rules info as qos_state
            QOS_STATE set rule_list "$rule_list "
            QOS_STATE set rule_num "0"

            lua /lib/ip_qos/state_gen.lua

            qos_rules_parse "$rule_list"
        fi

        qos_reset

        # set ready flag
        touch $QOS_READY
    }
}

qos_reload()
{
    # if qos is not enabled, skip reload
    [ ! -f $QOS_READY ] && {
        QOS_WRITE_LOG " qos_reload, qos function not start, skip operation."
        return
    }

    local op="$1"
    shift
    [ "$op" = "update" ] && {
        local update_list="$@"
    }
    [ "$op" = "add" ] && {
        local add_list="$@"
    }
    [ "$op" = "delete" ] && {
        local del_list="$@"
    }

    rule_get()
    {
        [ -z "$list" ] && list="$1" || list="$list $1"
    }

    local rule_list=$(qos_rulelist_get rule_get)

    # process dellist
    [ 0 -ne ${#del_list} ] && {
        local rule_num=$(QOS_STATE get rule_num)
        for rule in $del_list; do
            [ -f ${QOS_STOP_FLAG} ] && break

            QOS_WRITE_LOG " qos_reload, delete rules: ${rule}"
            [ "$(QOS_STATE get ${rule}.enable)" = "on" ] && qos_rule_uninstall $rule

            tobj=$(QOS_STATE get ${rule}.time)
            if [ ${#tobj} -ne 0 -a $tobj != "Any" -a $tobj != "SELF_DEFINE" ]; then
                QOS_WRITE_LOG " qos_reload, $rule delete timeobj $tobj"
                ret=$(timeobj_api_delete "$tobj" $THIS_MODULE $rule)
            fi
            QOS_STATE del "$rule"
            let rule_num--
        done
        ret=$(timeobj_api_commit $THIS_MODULE)
        QOS_STATE set rule_list "$rule_list "
        QOS_STATE set rule_num "$rule_num"
    }

    # process addlist
    [ 0 -ne ${#add_list} ] && {
        for rule in $add_list; do
            QOS_WRITE_LOG " qos_reload, add new rules: ${rule}"
            for opt in $RULE_OPT_LIST; do
                QOS_STATE set ${rule}.${opt} "$(config_get $rule $opt)"
            done
        done
        QOS_STATE set rule_list "$rule_list "

        qos_rules_parse "$add_list"
    }

    [ 0 -ne ${#update_list} ] && {
        for rule in $update_list; do
            qos_rule_uninstall $rule
        done

        for rule in $update_list; do
            [ "$(config_get $rule enable)" = "on" ] && {
                local timeobj
                local timeobj_bk=$(QOS_STATE get "${rule}.time")
                local status=$(QOS_STATE get "${rule}.enable")
                config_get timeobj "$rule" "time"
                QOS_WRITE_LOG " qos_reload, update rule, timeobj=$timeobj, timeobj_bk=$timeobj_bk"
                [ "$timeobj" = "" ] && timeobj="Any"

                [ "$status" = "on" -a "$timeobj_bk" != "Any" -a "$timeobj_bk" != "SELF_DEFINE" ] && {
                    QOS_WRITE_LOG " qos_reload, rule del from tmngtd, timeobj_bk=$timeobj_bk, rule=$rule."
                    ret=$(timeobj_api_delete "$timeobj_bk" $THIS_MODULE $rule)
                }

                [ "$timeobj" != "Any" ] && {
                    QOS_WRITE_LOG " qos_reload, rule reg to tmngtd, timeobj=$timeobj, rule=$rule."
                    [ "$timeobj" = "SELF_DEFINE" ] && {
                        # self define timeobj name is the same as rule name
                        timeobj=${rule}
                    }
                    ret=$(timeobj_api_add "$timeobj" $THIS_MODULE $rule)
                }

                [ "$timeobj" = "Any" ] && {
                    qos_rule_install $rule || {
                        QOS_WRITE_LOG " qos_reload, update rule, install rule $rule fail."
                    }
                }
            }

            [ "$(config_get $rule enable)" = "off" ] && {
                local timeobj_del=$(QOS_STATE get "${rule}.time")
                [ "$timeobj_del" != "Any" -a "$timeobj_del" != "SELF_DEFINE" ] && {
                    ret=$(timeobj_api_delete "$timeobj_del" $THIS_MODULE $rule)
                    QOS_WRITE_LOG " qos_reload, rule del from tmngtd, timeobj_del=$timeobj_del, rule=$rule."
                }
            }

            # update rule info in state.data
            for opt in $RULE_OPT_LIST; do
                QOS_STATE set "${rule}.${opt}" "$(config_get $rule $opt)"
            done

        done
        ret=$(timeobj_api_commit $THIS_MODULE)
        QOS_STATE set rule_list "$rule_list "
    }

    qos_reset
}

qos_time_handle()
{
    [ ! -f $QOS_READY ] && {
        QOS_WRITE_LOG " qos_time_handle, qos not config, no need time handle."
        return
    }

    local mode="$1"  # active or expire or reset
    local symbol="$2"  # time object name

    [ "$mode" = "ACTIVE" ] && cb=qos_rule_install || cb=qos_rule_uninstall
    for rule in $symbol; do
        [ -f ${QOS_STOP_FLAG} ] && break

        # check if rule exist in UCI?
        [ "`uci get ${THIS_MODULE}.${rule} 2>/dev/null`" != "rule" ]&& {
            QOS_WRITE_LOG " qos_time_handle, rule $rule not exist in UCI."
            continue
        }

        $cb $rule || {
            QOS_WRITE_LOG " qos_time_handle process $cb $rule fail."
            break
        }
    done

    qos_reset
    return 0
}

qos_ipgroup_handle()
{
    [ ! -f $QOS_READY ] && {
        QOS_WRITE_LOG " qos_ipgroup_handle, qos not config, no need ipgroup handle."
        return
    }
    local mode="$1" # update or delete
    local ipobj="$2"

    [ $ipobj = "IPGROUP_ANY" ] && return

    [ "$mode" != "update" ] && {
        QOS_WRITE_LOG " qos_ipgroup_handle, ipgroup handle mode != update."
        return
    }

    # compose the rule_list match the ipgroup obj
    rule_get()
    {
        local rule="$1"
        [ "$(config_get "$rule" ip_group)" = "$ipobj" ] && list="$list $rule"
    }
    local rule_list=$(qos_rulelist_get rule_get)
    [ ${#rule_list} -eq 0 ] && {
        QOS_WRITE_LOG " qos_ipgroup_handle, No rule match ipgroup $ipobj"
        return
    }

    QOS_WRITE_LOG " qos_ipgroup_handle, ipgroup $ipobj apply by rule_list:$rule_list"
    for rule in $rule_list; do
        [ -f ${QOS_STOP_FLAG} ] && break

        qos_rule_uninstall $rule && qos_rule_install $rule || QOS_WRITE_LOG " qos_ipgroup_handle, uninstall rule $rule fail."
    done

    qos_reset
}

qos_ifup_handle()
{
    local iface="$1"
    local dev=""
    if [ $iface = "lan" ]; then
        dev="$LAN_NETDEV_2"

    elif [ $WAN_MODE_ONE = "1" ]; then
        dev="$WANONE_NETDEV_2"

    else
        dev=$(zone_get_device_byif $iface)
        local zone=$(zone_get_zone_byif $iface)
        local isExist=`iptables -w -t mangle -S qos_chain_default | grep -w "$dev"`
        [ ${#isExist} -eq 0 ] && {
            rule_get()
            {
                local rule="$1"
                [ "$(config_get "$rule" if_pong)" = "$zone" ] && list="$list $rule"
            }
            local rule_list=$(qos_rulelist_get rule_get)
            [ ${#rule_list} -ne 0 ] && qos_nf_ifup_reload "$dev" "$rule_list"
        }

        dev=$(zone_get_bind_device ${zone})
    fi

    [ $(qos_tc_dev_check_root $dev) -eq 0 ] && {
        qos_reset
    }
}

qos_ifdown_handle()
{
    echo -e "do nothing."
}

if [ "$1" = "stop" ]; then
    touch ${QOS_STOP_FLAG}
fi

{
    flock -x 25
    local args="$@"
    if [ "$1" = "stop" ]; then
        shift
        qos_destroy "$@"

    elif [ "$1" = "start" ]; then
        shift
        qos_start "$@"

    elif [ "$1" = "reload" ]; then
        shift
        local op="$1"
        shift
        local op_list="$@"
        qos_reload "$op" "$op_list"

    elif [ "$1" = "time" ]; then
        local mode="$2"
        shift 2
        local symbol="$@"
        qos_time_handle "$mode" "$symbol"

    elif [ "$1" = "ipgroup" ]; then
        local mode="$2"
        local obj="$3"
        qos_ipgroup_handle $mode $obj

    elif [ "$1" = "ifup" ]; then
        local symbol="$2"
        qos_ifup_handle $symbol

    elif [ "$1" = "ifdown" ]; then
        local symbol="$2"
        qos_ifdown_handle $symbol

    else
        echo -e "$0 not support arg $args"
    fi

    flock -u 25
} 25<> $QOS_LOCK