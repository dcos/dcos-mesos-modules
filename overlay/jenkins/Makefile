base: Dockerfile_base
	docker build -t mesosphere/base-mesos-overlay-modules -f Dockerfile_base .
jenkins-image: Dockerfile base
	docker build -t mesosphere/ci-mesos-overlay-modules -f Dockerfile .
push:
	docker push mesosphere/ci-mesos-overlay-modules:latest
