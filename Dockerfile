FROM centos:7
MAINTAINER everyonce

# install initial dev toolchain
RUN yum update -y && yum groupinstall -y 'Development Tools' 
RUN yum install -y ruby perl-Digest epel-release ccache python cmake3 ctest3 which
RUN yum update -y 
RUN yum install -y python-pip

# fix cmake/test per yugabyte instructions
RUN ln -s /usr/bin/cmake3 /usr/local/bin/cmake
RUN ln -s /usr/bin/ctest3 /usr/local/bin/ctest

# add new user and home dir
RUN useradd -ms /bin/bash yugabyte
WORKDIR /home/yugabyte
RUN chown -R yugabyte:yugabyte .
USER yugabyte

# Add brew prereqs
RUN git clone https://github.com/linuxbrew/brew.git ~/.linuxbrew-yb-build
RUN ~/.linuxbrew-yb-build/bin/brew install autoconf automake boost flex gcc libtool openssl libuuid maven cmake

# Add codebase
ADD ./ /home/yugabyte/code/
WORKDIR /home/yugabyte/code

# fix codebase ownership
USER root
RUN chown -R yugabyte:yugabyte .

# build yugabytedb
USER yugabyte
RUN USER=$(whoami) ./yb_build.sh release --with-assembly
RUN USER=$(whoami) ./yb_release --edition ce --build_archive


# start official yugabyte docker image creation here

FROM centos:7
MAINTAINER YugaByte
ENV container=yugabyte-db

ENV YB_HOME=/home/yugabyte
WORKDIR $YB_HOME

COPY --from=0 /home/yugabyte/code/build/yugabyte*.tar.gz /tmp/
RUN tar -xvf /tmp/yugabyte*.tar.gz --strip 1
RUN bin/configure

# Expose the required ports.
EXPOSE 7000 7100 9000 9100 12000 11000 6379 9042 10100

# Create the data directories.
VOLUME ["/mnt/disk0", "/mnt/disk1"]

