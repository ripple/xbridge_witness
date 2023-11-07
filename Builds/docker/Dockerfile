FROM centos:7 as builder

ARG CMAKE_VERSION=3.27.7
ARG GIT_VERSION=2.42.0
# https://github.com/docker/buildx/issues/379
RUN ulimit -n 1024000 && yum update -y

COPY ./setup_centos.sh /tmp/setup_centos.sh
RUN /tmp/setup_centos.sh

COPY ./build_git.sh /tmp/build_git.sh
RUN /tmp/build_git.sh ${GIT_VERSION}

COPY ./install_cmake.sh /tmp/install_cmake.sh
RUN /tmp/install_cmake.sh ${CMAKE_VERSION}
