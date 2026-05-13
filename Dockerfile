# Base images 基础镜像
FROM ubuntu:22.04

# Set Timezone CST
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai

COPY sources.list /etc/apt/sources.list
RUN apt-get update && \
    apt-get install -y tzdata && \
    ln -fs /usr/share/zoneinfo/$TZ /etc/localtime && \
    dpkg-reconfigure --frontend noninteractive tzdata && \
    apt-get install -y \
        vim \
        iproute2 \
        traceroute \
        autoconf \
        make \
        gcc \
        libpcap-dev \
        openssh-server \
        openssh-client \
        iputils-ping \
        net-tools \
        tcpdump && \
    rm -rf /var/lib/apt/lists/*

CMD ["tail", "-f", "/dev/null"]

