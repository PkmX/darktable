#!/usr/bin/env bash

set -e
set -u

function print_usage() {
    echo 'Usage: haldclut2dtstyle [-c|--chartsize 4|5] [-n|--num_patches n] [-h | --help] [haldclut-file] ...' 1>&2
    exit 1
}

(( $# == 0 )) && print_usage

opts=$(getopt -o 'c:n:h' -l 'chartsize:,num_patches:,help' -n 'haldclut2dtstyle' -- "$@")
(( $? != 0 )) && exit 1
eval set -- "$opts"

chartsize=4
num_patches=49

while true; do
    case "$1" in
        -c|--chartsize)
            shift;
            chartsize=$1
            if (( ${chartsize} != 4 && ${chartsize} != 5)); then
                echo 'Only chart size of 4 or 5 are supported.' 1>&2
                exit 1
            fi
            shift;
            ;;
        -n|--num_patches)
            shift;
            num_patches=$1
            if (( ${num_patches} < 24 || ${num_patches} > 49)); then
                echo 'Number of patches must be between 24 and 49.' 1>&2
                false
            fi
            shift;
            ;;
        -h|--help)
            print_usage
            ;;
        --)
            shift;
            break;
            ;;
    esac
done

if (( $# >= 1 )); then
    workdir=$(mktemp -d)
    trap "rm -r ${workdir}" EXIT

    convert \( \( "hald:${chartsize}" \) \( -size 32x1 gradient: \) -append -depth 8 -colorspace sRGB \) -filter point -resize 200% "${workdir}/identity.png"
    darktable-cli "${workdir}/identity.png" to-lab.xmp "${workdir}/identity.pfm"
    for f in "$@"; do
        convert "${workdir}/identity.png" "${f}" -hald-clut "${workdir}/output.png"
        darktable-cli "${workdir}/output.png" to-lab.xmp "${workdir}/output.pfm"
        style_name=$(basename "${f}")
        darktable-chart "${workdir}/identity.pfm" "haldclut${chartsize}.cht" "${workdir}/output.pfm" 1 "${num_patches}" "${f%.png}.dtstyle" "${style_name}" "${style_name}"
        rm -f "${workdir}/output.pfm"
    done
else
    print_usage
fi
