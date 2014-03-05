#!/bin/bash
#
#  tc uses the following units when passed as a parameter.
#  kbps: Kilobytes per second 
#  mbps: Megabytes per second
#  kbit: Kilobits per second
#  mbit: Megabits per second
#  bps: Bytes per second 
#       Amounts of data can be specified in:
#       kb or k: Kilobytes
#       mb or m: Megabytes
#       mbit: Megabits
#       kbit: Kilobits
#  To get the byte figure from bits, divide the number by 8 bit
#
TC=/sbin/tc
IF=em1		    # Interface 
DNLD=$3kbps          # DOWNLOAD Limit
UPLD=$3kbps         # UPLOAD Limit 
IP=128.112.139.204     # Host IP
port=$2
U32="$TC filter add dev $IF protocol ip parent 1:0 prio 1 u32"

start(){
    $TC qdisc add dev $IF root handle 1: htb default 30
}

update(){

    #port=$2
    class=`echo "$port % 9999" | bc` 	
    echo $port $class
  #  $TC class del dev $IF parent 1: classid 1:$class
    $TC class replace dev $IF parent 1: classid 1:$class htb rate $DNLD
    $U32 match ip src $IP/32 match ip dport $port 0xffff flowid 1:$class
}

stop() {

    $TC qdisc del dev $IF root

}

restart() {

    stop
   #sleep 1
    start

}

show() {

    $TC -s qdisc ls dev $IF

}

case "$1" in

  start)

    echo -n "Starting bandwidth shaping: "
    start
    echo "done"
    ;;

  stop)

    echo -n "Stopping bandwidth shaping: "
    stop
    echo "done"
    ;;

  restart)

    echo -n "Restarting bandwidth shaping: "
    restart
    echo "done"
    ;;

  show)
    	    	    
    echo "Bandwidth shaping status for $IF:\n"
    show
    echo ""
    ;;

  update)
    update
    ;;

  *)

    pwd=$(pwd)
    echo "Usage: $(/usr/bin/dirname $pwd)/tc.bash {start|stop|restart|show}"
    ;;

esac

exit 0


