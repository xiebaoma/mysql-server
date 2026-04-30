# mysql-server

cd /home/xiebaoma.xbm/mysql-server

mkdir -p build-local local/mysql local/data local/run local/mysql/conf

cmake -S . -B build-local \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/local/mysql" \
  -DMYSQL_DATADIR="$PWD/local/data" \
  -DSYSCONFDIR="$PWD/local/mysql/conf"

cmake --build build-local -j"$(nproc)"

vi ./local/mysql/conf/my.cnf

[mysqld]
basedir=/home/xiebaoma.xbm/mysql-server/local/mysql
datadir=/home/xiebaoma.xbm/mysql-server/local/data
socket=/home/xiebaoma.xbm/mysql-server/local/run/mysql.sock
pid-file=/home/xiebaoma.xbm/mysql-server/local/run/mysqld.pid
log-error=/home/xiebaoma.xbm/mysql-server/local/run/error.log
port=54000


./build-local/bin/mysqld --defaults-file="$PWD/local/mysql/conf/my.cnf" --initialize-insecure
./build-local/bin/mysqld --defaults-file="$PWD/local/mysql/conf/my.cnf" --daemonize

./build-local/bin/mysql -uroot -S "$PWD/local/run/mysql.sock"