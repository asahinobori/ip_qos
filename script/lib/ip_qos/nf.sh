#!/bin/sh

[ "$INC_QOS_NF" != "" ] && return
export INC_QOS_NF="INC"

local IPT=iptables
local TABLE=mangle
local IPT_PREFIX="$IPT -w -t $TABLE"
local QOS_CHAIN="qos_chain_default"

qos_nf_destroy()
{
    local destroy_param="$@"
    local chain="$QOS_CHAIN"

    # flush qos chain
    local ln=`$IPT_PREFIX -L FORWARD --line | grep -w "$chain" | awk '{print $1}'`
    [ ${#ln} -ne 0 ] && $IPT_PREFIX -D FORWARD $ln
    $IPT_PREFIX -F $chain 2>/dev/null
    $IPT_PREFIX -X $chain 2>/dev/null

    # flush sfe
    xfwdm -C > /dev/null 2>&1

    # flush self define ipgroup in ipset
    local ipgroup_list=`ipset list -n | grep -w '^qos_.*'`
    for ip_group in $ipgroup_list; do
        `ipset destroy ${ip_group} 2>/dev/null`
    done

    QOS_WRITE_LOG " qos_nf_destroy, flush $chain."
}

qos_nf_add_rule()
{
    local rule_name=$1
    local if_src=$2
    local if_dst=$3
    local flag=$4
    local ip_or_set=$5
    local id=$6
    local dir=$7

    local mask=$QOS_MARK_RULE_MASK_HEX
    local rule_mask=$QOS_MARK_RULE_MASK_HEX
    local chain="$QOS_CHAIN"

    # create qos_chain_default
    $IPT_PREFIX  -L FORWARD --line | grep -q -w "$chain" || {
        # create $chain
        QOS_WRITE_LOG " info, create qos default chain: $chain"
        QOS_WRITE_LOG " $IPT_PREFIX -N $chain 2>/dev/null"
        $IPT_PREFIX -N $chain 2>/dev/null

        # connmark restore
        QOS_WRITE_LOG " $IPT_PREFIX -A $chain -j CONNMARK --restore-mark --nfmask $mask --ctmask $mask"
        $IPT_PREFIX -A $chain -j CONNMARK --restore-mark --nfmask $mask --ctmask $mask

        # anti-reentry mechanism
        QOS_WRITE_LOG " $IPT_PREFIX -A $chain -m mark ! --mark 0/$rule_mask -j RETURN"
        $IPT_PREFIX -A $chain -m mark ! --mark 0/$rule_mask -j RETURN

        # connmark save
        QOS_WRITE_LOG " $IPT_PREFIX -A $chain -j CONNMARK --save-mark --nfmask $mask --ctmask $mask"
        $IPT_PREFIX -A $chain -j CONNMARK --save-mark --nfmask $mask --ctmask $mask

        # create rule in FORWARD chain, and target it to $chain
        local add_method="-A FORWARD"
        ln=`$IPT_PREFIX  -L FORWARD --line | awk '/zone.*MSSFIX/ {print $1}' | awk 'NR==1'`
        [ ${#ln} -ne 0 ] && add_method="-I FORWARD $ln"
        QOS_WRITE_LOG " $IPT_PREFIX $plus -j $chain"
        $IPT_PREFIX $add_method -j $chain
    }


    local mt_netdev=""
    local mt_landev=""
    [ $if_dst != "WAN_ALL" -a $if_dst != "LAN" ] && mt_netdev="-o $(indirect_var_get ${if_dst}_dev)"
    [ $if_src != "WAN_ALL" -a $if_src != "LAN" ] && mt_netdev="-i $(indirect_var_get ${if_src}_dev) $mt_netdev"
    [ $if_dst = "LAN" ] && mt_landev="-m iface_group --dev_set LAN_IFACES"
    [ $if_src = "LAN" ] && mt_landev="-m iface_group --dev_set LAN_IFACES --iface_in 1"

    local ip_type=$(config_get $rule "ip_type")
    [ "$ip_type" != "src" -a "$ip_type" != "dst" ] && ip_type="src"
    [ $if_dst = "$LAN_IFACE_NAME" ] && ip_type="dst"

    local mt_ipset="-m set --match-set ${ip_or_set} $ip_type"
    [ ${ip_or_set} = "IPGROUP_ANY" ] && {
        # IPGROUP_ANY, mt_ipset no need
        QOS_WRITE_LOG " CAUTION, $rule_name is a IPGROUP_ANY rule !"
        mt_ipset=""
    }

    local mt_mark="-m mark --mark 0/$rule_mask"
    local tg_qmark="-j qmark --set-mask $mask --set-id $id --set-dir $dir"

    lua /lib/ip_qos/find_index.lua ${rule_name/_mate/}

    local insert_index=`cat ${QOS_TMP_DIR}/.insert_index`
    let insert_index++
    let insert_index++ # conmark restore rule and re-entry return rule cost 2 index
    insert="-I $chain $insert_index"

    local comment="-m comment --comment $rule_name"

    QOS_WRITE_LOG " $IPT_PREFIX $insert $mt_netdev $mt_landev $mt_ipset $mt_mark $comment $tg_qmark"
    $IPT_PREFIX $insert $mt_netdev $mt_landev $mt_ipset $mt_mark $comment $tg_qmark

    return 0
}

qos_nf_del_rule()
{
    local rule_list="$@"
    local chain="$QOS_CHAIN"

    for rule_name in $rule_list; do
        local idx=`$IPT_PREFIX -nvL $chain --line |  grep -w "$rule_name " | awk '{print $1}'`
        [ ${#idx} -ne 0 ] && {
            $IPT_PREFIX -D $chain $idx

            # if $chain is empty, smash it and delete target in FORWARD chain
            [ `$IPT_PREFIX -S $chain | awk 'END{print NR}'` = 4 ] && {
                QOS_WRITE_LOG " qos_nf_del_rule, $chain is empty, should destroy."
                local i=`$IPT_PREFIX -L FORWARD --line | grep -w "$chain" | awk '{print $1}'`
                [ ${#i} -ne 0 ] && $IPT_PREFIX -D FORWARD $i
                $IPT_PREFIX -F $chain
                $IPT_PREFIX -X $chain
            }
        }
    done

    return 0
}

qos_nf_ifup_reload()
{
    local dev="$1"
    local rule_list="$2"
    local chain="$QOS_CHAIN"

    for rule_name in $rule_list; do
        local content=`$IPT_PREFIX -S $chain | awk -v var=$dev '/comment '$rule_name' / {printf (NR-1)" "$3" "var" ";for(i=5;i<=NF;i++) printf $i" ";}'`
        [ ${#content} -ne 0 ] && $IPT_PREFIX -R $chain $content

        local content_mate=`$IPT_PREFIX -S $chain | awk -v var=$dev '/comment '$rule_name'_mate / {printf (NR-1)" "$3" "var" ";for(i=5;i<=NF;i++) printf $i" ";}'`
        [ ${#content_mate} -ne 0 ] && $IPT_PREFIX -R $chain $content_mate
    done

    return 0
}