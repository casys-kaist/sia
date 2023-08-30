#!/bin/bash

CURRENT_PATH="."

function traverse() {
    echo "Entering ${1}"
    for file in `ls $1`
    do
        if [ ! -d ${1}/${file} ]; then
            if [[ ${file} == *.v ]] || [[ ${file} == *.sv ]]; then
                echo "Found rtl file ${1}/${file}"
                cp ${1}/${file} ${CURRENT_PATH}/${file}
                echo "${file}" >> ${CURRENT_PATH}/sources.txt
            fi
        else
            echo "C:${CURRENT_PATH}/${file}/sources.txt" >> ${CURRENT_PATH}/sources.txt

            mkdir -p ${CURRENT_PATH}/${file}
            OLD_PATH=${CURRENT_PATH}
            CURRENT_PATH=${CURRENT_PATH}/${file}
            touch ${CURRENT_PATH}/sources.txt
            
            traverse "${1}/${file}"

            CURRENT_PATH=${OLD_PATH}
        fi
    done
}

echo "Get verilog files from $1"
touch ${CURRENT_PATH}/sources.txt
traverse $1