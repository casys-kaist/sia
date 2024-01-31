TE=4
PU=2
PE=3
echo "generating $TE, $PU, $PE"
cp -r hw_te_${TE} design_${TE}_${PU}_${PE}
cp ../../../generated/${PU}_${PE}/QRD_Unit.v ./design_${TE}_${PU}_${PE}/

afu_synth_setup -s design_${TE}_${PU}_${PE}/sources.txt build_${TE}_${PU}_${PE}
pushd build_${TE}_${PU}_${PE}
    qsub-synth
popd