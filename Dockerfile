FROM centos:7
RUN yum update -y && yum groupinstall -y 'Development Tools' && yum install -y ruby perl-Digest epel-release ccache python python-pip cmake3 ctest3 which
RUN ln -s /usr/bin/cmake3 /usr/local/bin/cmake
RUN ln -s /usr/bin/ctest3 /usr/local/bin/ctest
RUN useradd -ms /bin/bash yugabyte
WORKDIR /home/yugabyte
ADD ./ ./
RUN chown -R yugabyte:yugabyte .
USER yugabyte
RUN git clone https://github.com/linuxbrew/brew.git ~/.linuxbrew-yb-build
RUN ~/.linuxbrew-yb-build/bin/brew install autoconf automake boost flex gcc libtool openssl libuuid maven cmake
RUN ./yb_build.sh release --with-assembly
RUN ./yb_release --edition ce

FROM centos:7
MAINTAINER YugaByte
ENV container=yugabyte-db

ENV YB_HOME=/home/yugabyte
WORKDIR $YB_HOME

COPY packages/yugabyte*.tar.gz /tmp
RUN tar -xvf /tmp/yugabyte*.tar.gz --strip 1
RUN bin/configure

# Expose the required ports.
EXPOSE 7000 7100 9000 9100 12000 11000 6379 9042 10100

# Create the data directories.
VOLUME ["/mnt/disk0", "/mnt/disk1"]
