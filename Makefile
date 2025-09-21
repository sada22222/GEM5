hmmer_retro_7016:
	export GCBV_REF_SO="/nfs/home/zhaozhi/workspace/NEMU/build/riscv64-nemu-interpreter-so" && \
	export GCB_RESTORER="" && \
	time ./build/RISCV/gem5.opt \
	./configs/example/xiangshan.py  \
	--arch-db-fromstart=True \
	--no-pf --no-l3cache --mem-type=SimpleMemory \
	--enable-arch-db --arch-db-file=./hmmer_retro_7016.db \
	--warmup-insts-no-switch 20000000 \
	--maxinsts 40000000 \
	--generic-rv-cpt=/nfs/share/zyy/spec06_gcc15_rv64gcbv_base_seg0_ff0_zbs1_autovec_NEMU__full/checkpoint-0-0-0/hmmer_retro/7016/_7016_0.018315_.zstd

libquantum_22045:
	export GCBV_REF_SO="/nfs/home/zhaozhi/workspace/NEMU/build/riscv64-nemu-interpreter-so" && \
	export GCB_RESTORER="" && \
	time ./build/RISCV/gem5.opt \
	./configs/example/xiangshan.py  \
	--arch-db-fromstart=True \
	--no-pf --no-l3cache --mem-type=SimpleMemory \
	--enable-arch-db --arch-db-file=./libquantum_22045.db \
	--warmup-insts-no-switch 20000000 \
	--maxinsts 40000000 \
	--generic-rv-cpt=/nfs/share/zyy/spec06_gcc15_rv64gcbv_base_seg5_ff0_zbs1_autovec_NEMU__full/checkpoint-0-0-0/libquantum/22045/_22045_0.127386_.zstd

hmmer_retro_11027:
	export GCBV_REF_SO="/nfs/home/zhaozhi/workspace/NEMU/build/riscv64-nemu-interpreter-so" && \
	export GCB_RESTORER="" && \
	time ./build/RISCV/gem5.opt \
	./configs/example/xiangshan.py  \
	--arch-db-fromstart=True \
	--no-pf --no-l3cache --mem-type=SimpleMemory \
	--enable-arch-db --arch-db-file=./hmmer_retro_11027.db \
	--warmup-insts-no-switch 20000000 \
	--maxinsts 40000000 \
	--generic-rv-cpt=/nfs/share/zyy/spec06_gcc15_rv64gcbv_base_seg0_ff0_zbs1_autovec_NEMU__full/checkpoint-0-0-0/hmmer_retro/24682/_24682_0.055044_.zstd