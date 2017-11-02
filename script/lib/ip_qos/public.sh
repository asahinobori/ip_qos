#!/bin/sh

[ "$INC_QOS_PUBLIC" != "" ] && return
export INC_QOS_PUBLIC="INC"

. /lib/functions.sh
. /lib/zone/zone_api.sh
. /lib/time_mngt/timeobj_api.sh

export THIS_MODULE="qos"
export STYPE_GLOBAL="global"
export SNAME_SETTING="setting"
export STYPE_IFACE="interface"
export STYPE_RULE="rule"

export LINERATE=1000000kbps
export RATE_MAX=1000000kbps

# common qos model
local QOS_RULE_SPEC=`uci -c /etc/profile.d get profile.@qos[0].rule_spec`
[ -z $QOS_RULE_SPEC ] && QOS_RULE_SPEC=20

export QOS_CELL_SPEC=`uci -c /etc/profile.d get profile.@qos[0].cell_spec`
[ -z $QOS_CELL_SPEC ] && QOS_CELL_SPEC=256

# devmngr model
local QOS_DM_RULE_SPEC=`uci -c /etc/profile.d get profile.@devmngr[0].devmngr_rule_max`
[ -z $QOS_DM_RULE_SPEC ] && QOS_DM_RULE_SPEC=256

# visitor(wireless) model
export VISITOR_IFACE=`uci get wireless.visitor.iface`
export VISITOR_DEV=`uci get wireless.visitor.mtk_iface`
[ ${#VISITOR_DEV} -eq 0 ] && {
    VISITOR_DEV=`uci get wireless.visitor.iface`
}

export QOS_MARK_BIT_START=11    # start bit for qos in nfmark
export QOS_MARK_BIT_LEN=10      # from 11 to 20 bit
local QOS_MARK_MASK_BITS_RAW=`awk "BEGIN{f=lshift(1, $QOS_MARK_BIT_LEN)-1; print f}"`
export QOS_MARK_MASK=`awk "BEGIN{f=lshift($QOS_MARK_MASK_BITS_RAW, $QOS_MARK_BIT_START); print f}"`
export QOS_MARK_MASK_HEX=`printf "0x%x" $QOS_MARK_MASK`

# 2bit form 19 to 20
# 00 for qos_common, 01 for devmngr, 10 for visitor(wireless)
local QOS_ADDITION_LEN=$((QOS_MARK_BIT_LEN - 2))
local QOS_DM_MARK_ADDITION_RAW=`awk "BEGIN{f=lshift(1, $QOS_ADDITION_LEN); print f}"`
local QOS_VT_MARK_ADDITION_RAW=`awk "BEGIN{f=lshift(2, $QOS_ADDITION_LEN); print f}"`
export QOS_COMMON_MARK_ADDITION=0
export QOS_DM_MARK_ADDITION=`awk "BEGIN{f=lshift($QOS_DM_MARK_ADDITION_RAW, $QOS_MARK_BIT_START); print f}"`
export QOS_VT_MARK_ADDITION=`awk "BEGIN{f=lshift($QOS_VT_MARK_ADDITION_RAW, $QOS_MARK_BIT_START); print f}"`

# rule mask, form 11 to 18 bit
local QOS_MARK_RULE_MASK_BITS_RAW=`awk "BEGIN{f=lshift(1, $QOS_ADDITION_LEN)-1; print f}"`
export QOS_MARK_RULE_MASK=`awk "BEGIN{f=lshift($QOS_MARK_RULE_MASK_BITS_RAW, $QOS_MARK_BIT_START); print f}"`
export QOS_MARK_RULE_MASK_HEX=`printf "0x%x" $QOS_MARK_RULE_MASK`

export QOS_CONFIG_DIR="/etc/config"
export QOS_UCI="${QOS_CONFIG_DIR}/${THIS_MODULE}"
export QOS_LIB_DIR="/lib/ip_qos"
export QOS_TMP_DIR="/tmp/ip_qos"
export QOS_LOG="${QOS_TMP_DIR}/debug.log"
export QOS_MARK_FILE="${QOS_TMP_DIR}/mark.data"
export QOS_DM_MARK_FILE="${QOS_TMP_DIR}/mark_dm.data"
export QOS_VT_MARK_FILE="${QOS_TMP_DIR}/mark_vt.data"
export QOS_READY="${QOS_TMP_DIR}/.ready"
export DEVMNGR_READY="${QOS_TMP_DIR}/.devmngr_ready"
export QOS_POLLING_FILE="${QOS_TMP_DIR}/.polling"
export QOS_STOP_FLAG="${QOS_TMP_DIR}/.stop_flag"
export QOS_LOCK="${QOS_TMP_DIR}/.lock"
[ -f $QOS_UCI ] || {
    touch $QOS_UCI
    export QOS_CONFIG_INVALID="true"
    return
}

[ ! -d $QOS_TMP_DIR ] && mkdir $QOS_TMP_DIR
[ ! -d ${QOS_TMP_DIR}/log ] && mkdir ${QOS_TMP_DIR}/log
[ ! -f $QOS_LOG ] && echo -e "time                  log_info" > "$QOS_LOG"

export QOS_WIRELESS=0
local radio_value=`grep -w "^radio" /etc/productinfo | awk -F : '{print $2}'`
[ $radio_value -gt 0 ] && export QOS_WIRELESS=1

# load qos config
config_load "$THIS_MODULE"

export QOS_THRESHOLD=$(config_get "$SNAME_SETTING" qos_threshold)
[ -z $QOS_THRESHOLD ] && QOS_THRESHOLD=0

# log api
QOS_RET()
{
    echo "$1"
}

QOS_WRITE_FILE()
{
    echo -e "$1" >> "$2" 2>/dev/null
}

QOS_WRITE_LOG()
{
    local log="$1"
    local file=$QOS_LOG
    local time=`date +%Y%m%d-%H.%M.%S`
    local item="${time}    ${log}"
    # if log size > 150K, save and recreate
    [ `ls -l "$file" | awk '{print $5}'` -gt 100000 ] && {
        # keep count of logfile below 10
        [ `ls ${QOS_TMP_DIR}/log/ | wc -l` -ge 10 ] && {
            rm ${QOS_TMP_DIR}/log/`ls ${QOS_TMP_DIR}/log | awk 'NR==1 {print $1}'`
        }
        mv "$file" "${QOS_TMP_DIR}/log/${time}_qos.log"  2>/dev/null
        QOS_WRITE_FILE "time\t\t\tlog_info" "$file"
    }
    echo -e "$item" >> "$file" 2>/dev/null
}

# state operation
export QOS_STATE_FILE="${QOS_TMP_DIR}/state.data"
[ ! -f $QOS_STATE_FILE ] && touch $QOS_STATE_FILE
qos_get_state()
{
    local key="$1"
    grep "^${key}.*" "$QOS_STATE_FILE" 2>&1 >/dev/null || {
        return 1
    }
    QOS_RET "`awk -F= "/^${key}/" $QOS_STATE_FILE | awk -F= '{print $2}'`"
}
qos_set_state()
{
    local key="$1"
    local cont="$2"
    grep "^${key}.*" "$QOS_STATE_FILE" 2>&1 >/dev/null || {
        QOS_WRITE_FILE "${key}=${cont}" "$QOS_STATE_FILE"
        return 0
    }
    sed "/^${key}/ s/=.*$/=${cont}/" -i "$QOS_STATE_FILE"
}
qos_delete_state()
{
    local key="$1"
    # CAUTION: Actually, iface and rule need call this handle. in case of name-include(r1<r11), must set the pattern with '.'
    grep "^${key}\..*" $QOS_STATE_FILE 2>&1 >/dev/null && sed "/^${key}\..*/d" -i $QOS_STATE_FILE
}
qos_clear_state()
{
    rm -rf $QOS_STATE_FILE 2>/dev/null
    touch $QOS_STATE_FILE
}
# qos state operate handle
QOS_STATE()
{
    local cmd="$1"
    shift
    case "$cmd" in
        new)
            qos_set_state "$1"  "$1_state"
        ;;
        clear)
            qos_clear_state
        ;;
        set)
            qos_set_state "$1" "$2"
        ;;
        get)
            QOS_RET "$(qos_get_state "$1")"
        ;;
        del)
            qos_delete_state "$1"
        ;;
    esac;
}

# check the netdev is valid or not
valid_dev()
{
    local mydev="$1"
    [ ${#mydev} -eq 0 ] && QOS_RET "2" && return
    `ifconfig | grep -w "Link encap" | awk '{print $1}' | grep -w -q "$mydev"`
    QOS_RET "$?"
}

get_iface_linerate()
{
    local iface=$1
    local link=$2
    config_get linerate "$iface" "$link"
    # if linerate not exist, use default line-rate which is set in public.sh
    [ ${#linerate} -eq 0 -o `echo $linerate|sed 's/[a-zA-Z]//g'` -eq 0 ] && linerate=$(indirect_var_get ${iface}_linerate)
    linerate="`echo $linerate | sed 's/[^0-9]//g'`kbps" # unit kbps
    QOS_RET "$linerate"
}

# get val from indirect var
indirect_var_get()
{
    local raw=$1
    #variable name can not include char like '.' and '-', so change to '_'
    local raw=`echo ${raw//"."/"_"}`
    local raw=`echo ${raw//"-"/"_"}`
    eval val=\${$raw}
    QOS_RET "$val"
}

# ser val to a indirect var
indirect_var_set()
{
    local raw="$1"
    #variable name can not include char like '.' and '-', so change to '_'
    local raw=`echo ${raw//"."/"_"}`
    local raw=`echo ${raw//"-"/"_"}`
    local val="$2"
    eval "$raw=$val"
}

dec2hex()
{
    [ `echo $1 | grep -c "0x"` -ne 0 ] && QOS_RET "$1" && return
    QOS_RET `printf 0x%x $1`
}

hex2dec()
{
    [ `echo $1 | grep -c "0x"` -eq 0 ] && QOS_RET "$1" && return
    QOS_RET `printf %d $1`
}

qos_uci_ifaces_get()
{
    local list=""
    config_get list "$SNAME_SETTING" "$STYPE_IFACE"
    QOS_RET "$list"
}

export LAN_IFACE_NAME="LAN"
export WAN_MODE_ONE_IFACE_NAME="WAN1"
export SINGLE_WAN_IFACE_NAME="WAN"
export IFACE_LIST=$(qos_uci_ifaces_get)
export ZONE_LIST="$IFACE_LIST"
export DEV_LIST=""
local cnt=1
for iface in $ZONE_LIST; do
    local dev=$(zone_get_effect_devices $iface)
    indirect_var_set ${iface}_dev "$dev"
    [ -z "$DEV_LIST" ] && DEV_LIST="$dev" || DEV_LIST="$DEV_LIST $dev"
    indirect_var_set ${iface}_rootid $cnt
    indirect_var_set ${iface}_linerate $LINERATE
    cnt=$((cnt+1))
done
export LAN_IFACE=`uci get zone.${LAN_IFACE_NAME}.if_eth`
export LAN_NETDEV=$(indirect_var_get ${LAN_IFACE_NAME}_dev)
export LAN_NETDEV_2=$(zone_get_bind_device ${LAN_IFACE_NAME})
export SINGLE_WAN=`uci get network.@interface_mode[0].singlewan`
[ -z $SINGLE_WAN ] && SINGLE_WAN=0
export WAN_MODE_ONE=`uci get network.@interface_mode[0].wanmode`
[ -z $WAN_MODE_ONE ] && WAN_MODE_ONE=0
if [ $SINGLE_WAN = "1" ]; then
    export WANONE_NETDEV=$(indirect_var_get ${SINGLE_WAN_IFACE_NAME}_dev)
    export WANONE_NETDEV_2=$(zone_get_bind_device ${SINGLE_WAN_IFACE_NAME})
else
    export WANONE_NETDEV=$(indirect_var_get ${WAN_MODE_ONE_IFACE_NAME}_dev)
    export WANONE_NETDEV_2=$(zone_get_bind_device ${WAN_MODE_ONE_IFACE_NAME})
fi