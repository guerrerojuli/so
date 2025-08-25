FROM agodio/itba-so-multi-platform:3.0

# Instalar dependencias para compilar con ncurses
RUN apt-get update && \
    apt-get install -y --no-install-recommends libncurses-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /sotp


