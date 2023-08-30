#!/bin/bash

TES=(3 3 3 2 2 1 1 1 1 4 4)
PUS=(1 8 4 3 4 12 6 4 2 2 3)
PES=(8 1 2 4 3 2 4 6 12 3 2)

for i in "${!TES[@]}"; do
    TE=${TES[i]}
    PU=${PUS[i]}
    PE=${PES[i]}
    echo "generating $TE, $PU, $PE"
    cp -r hw_te_${TE} design_${TE}_${PU}_${PE}
    cp ../../../generated/${PU}_${PE}/QRD_Unit.v ./design_${TE}_${PU}_${PE}/

    afu_synth_setup -s design_${TE}_${PU}_${PE}/sources.txt build_${TE}_${PU}_${PE}
    pushd build_${TE}_${PU}_${PE}
        qsub-synth
    popd
done