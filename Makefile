setup_4:
	./script/setup.sh 4 2 2

setup_16:
	./script/setup.sh 16 2 2

setup_64:
	./script/setup.sh 64 8 4

test:
	./script/test.sh

logs:
	./script/logs.sh

clean:
	./script/clean.sh
