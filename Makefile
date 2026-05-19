.PHONY: setup_env simple_16 medium_16 complex_16 clean

setup_env:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "Please run: sudo make setup_env"; \
		exit 1; \
	fi
	apt-get update
	DEBIAN_FRONTEND=noninteractive apt-get install -y \
		docker.io \
		python3 \
		libpcap-dev
	docker --version
	python3 --version
	mkdir -p /etc/docker
	cp daemon.json /etc/docker/daemon.json
	systemctl daemon-reload
	systemctl stop docker.service
	systemctl stop docker.socket
	systemctl start docker.service
	systemctl start docker.socket


simple_16:
	./script/setup.sh 16 2 2
	./script/test.sh simple_16 -b
	./script/logs.sh simple_16_b.txt
	./script/restart.sh -i
	./script/logs.sh simple_16_i.txt
	./script/restart.sh -r
	./script/logs.sh simple_16_r.txt
	./script/restart.sh -t
	./script/logs.sh simple_16_t.txt
	python3 ./script/plot_switch_forwarding.py \
		result/simple_16_b.txt \
		result/simple_16_i.txt \
		result/simple_16_r.txt \
		result/simple_16_t.txt \
		-o assets/simple_16.svg

medium_16:
	./script/setup.sh 16 2 2
	./script/test.sh medium_16 -b
	./script/logs.sh medium_16_b.txt
	./script/restart.sh -i
	./script/logs.sh medium_16_i.txt
	./script/restart.sh -r
	./script/logs.sh medium_16_r.txt
	./script/restart.sh -t
	./script/logs.sh medium_16_t.txt
	python3 ./script/plot_switch_forwarding.py \
		result/medium_16_b.txt \
		result/medium_16_i.txt \
		result/medium_16_r.txt \
		result/medium_16_t.txt \
		-o assets/medium_16.svg

complex_16:
	./script/setup.sh 16 2 2
	./script/test.sh complex_16 -b
	./script/logs.sh complex_16_b.txt
	./script/restart.sh -i
	./script/logs.sh complex_16_i.txt
	./script/restart.sh -r
	./script/logs.sh complex_16_r.txt
	./script/restart.sh -t
	./script/logs.sh complex_16_t.txt
	python3 ./script/plot_switch_forwarding.py \
		result/complex_16_b.txt \
		result/complex_16_i.txt \
		result/complex_16_r.txt \
		result/complex_16_t.txt \
		-o assets/complex_16.svg

clean:
	./script/clean.sh
