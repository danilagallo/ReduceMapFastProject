#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <commons/collections/list.h>
#include <commons/log.h>
#include <commons/config.h>
#include <commons/string.h>
#include <commons/bitarray.h>


//Includes para mongo
#include <bson.h>
#include <mongoc.h>
#include "FS_MDFS.h"

//Variables globales
fd_set master; // conjunto maestro de descriptores de fichero
fd_set read_fds; // conjunto temporal de descriptores de fichero para select()
t_log* logger;
t_list *nodos; //lista de nodos conectados al fs
t_list* archivos; //lista de archivos del FS
t_list* directorios; //lista de directorios del FS
t_bloque* bloque; //Un Bloque del Archivo
t_config * configurador;
t_archivo* unArchivo; //Un archivo de la lista de archivos del FS
t_bloque *unBloque;
t_copias *unaCopia;
int fdmax; // número máximo de descriptores de fichero
int listener; // descriptor de socket a la escucha
struct sockaddr_in filesystem; // dirección del servidor
struct sockaddr_in remote_client; // dirección del cliente
char identificacion[BUF_SIZE]; // buffer para datos del cliente
char mensaje[MENSAJE_SIZE];
int cantidad_nodos = 0;
int cantidad_nodos_historico = 0;
int read_size;
int *bloquesTotales; //tendra la cantidad de bloques totales del file de datos
int marta_presente = 0; //Valiable para controlar que solo 1 proceso marta se conecte
int marta_sock;
char indiceDirectorios[MAX_DIRECTORIOS]; //cantidad maxima de directorios
int directoriosDisponibles; //reservo raiz
int j; //variable para recorrer el vector de indices
int *puerto_escucha_nodo;
char nodo_id[6];
char id_nodo[6];
t_nodo *nodosMasLibres[3];
char bufBloque[BLOCK_SIZE];
//Variables para la persistencia con mongo
mongoc_client_t *client;
mongoc_collection_t *collection;
mongoc_cursor_t *cursor;
bson_error_t error;
bson_oid_t oid;
bson_t *doc;

int main(int argc, char *argv[]) {

	pthread_t escucha; //Hilo que va a manejar los mensajes recibidos
	int newfd;
	int addrlen;
	int yes = 1; // para setsockopt() SO_REUSEADDR, más abajo
	configurador = config_create("resources/fsConfig.conf"); //se asigna el archivo de configuración especificado en la ruta
	logger = log_create("fsLog.log", "FileSystem", false, LOG_LEVEL_INFO);
	FD_ZERO(&master); // borra los conjuntos maestro y temporal
	FD_ZERO(&read_fds);

	mongoc_init();
	client = mongoc_client_new("mongodb://localhost:27017/");
	collection = mongoc_client_get_collection(client, "NODOS", "lista_nodos");
	bson_oid_init(&oid, NULL);

	if ((listener = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		log_error(logger, "FALLO la creacion del socket");
		exit(-1);
	}
	// obviar el mensaje "address already in use" (la dirección ya se está usando)
	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
		perror("setsockopt");
		log_error(logger, "FALLO la ejecucion del setsockopt");
		exit(-1);
	}
	// enlazar
	filesystem.sin_family = AF_INET;
	filesystem.sin_addr.s_addr = INADDR_ANY;
	filesystem.sin_port = htons(config_get_int_value(configurador, "PUERTO_LISTEN"));
	memset(&(filesystem.sin_zero), '\0', 8);
	if (bind(listener, (struct sockaddr *) &filesystem, sizeof(filesystem))	== -1) {
		perror("bind");
		log_error(logger, "FALLO el Bind");
		exit(-1);
	}
	// escuchar
	if (listen(listener, 10) == -1) {
		perror("listen");
		log_error(logger, "FALLO el Listen");
		exit(1);
	}
	// añadir listener al conjunto maestro
	FD_SET(listener, &master);

	// seguir la pista del descriptor de fichero mayor
	fdmax = listener; // por ahora es éste el ultimo socket
	addrlen = sizeof(struct sockaddr_in);
	nodos = list_create(); //Crea la lista de que va a manejar la lista de nodos
	printf("Esperando las conexiones de los nodos iniciales\n");
	log_info(logger, "Esperando las conexiones de los nodos iniciales");
	while (cantidad_nodos!= config_get_int_value(configurador, "CANTIDAD_NODOS")) {
		if ((newfd = accept(listener, (struct sockaddr*) &remote_client,(socklen_t*) &addrlen)) == -1) {
			perror("accept");
			log_error(logger, "FALLO el ACCEPT");
			exit(-1);
		}
		if ((read_size = recv(newfd, identificacion, 50, 0)) == -1) {
			perror("recv");
			log_error(logger, "FALLO el RECV");
			exit(-1);
		}
		if (read_size > 0 && strncmp(identificacion, "nuevo", 5) == 0) {
			bloquesTotales = malloc(sizeof(int));
			//Segundo recv, aca espero recibir la capacidad del nodo
			if ((read_size = recv(newfd, bloquesTotales, sizeof(int), 0))== -1) {
				perror("recv");
				log_error(logger, "FALLO el RECV");
				exit(-1);
			}
			puerto_escucha_nodo = malloc(sizeof(int));
			if ((read_size = recv(newfd, puerto_escucha_nodo, sizeof(int), 0))== -1) {
				perror("recv");
				log_error(logger, "FALLO el RECV");
				exit(-1);
			}
			if ((read_size = recv(newfd, nodo_id, sizeof(nodo_id), 0)) == -1) {
				perror("recv");
				log_error(logger, "FALLO el RECV");
				exit(-1);
			}
			if (read_size > 0) {
				if (validar_nodo_nuevo(nodo_id) == 0) {
					cantidad_nodos++;
					cantidad_nodos_historico = cantidad_nodos;
					FD_SET(newfd, &master); // añadir al conjunto maestro
					if (newfd > fdmax) { // actualizar el máximo
						fdmax = newfd;
					}
					list_add(nodos,agregar_nodo_a_lista(nodo_id, newfd, 0, 1,inet_ntoa(remote_client.sin_addr),remote_client.sin_port,*puerto_escucha_nodo, *bloquesTotales,*bloquesTotales));
					printf("Se conectó un nuevo nodo: %s con %d bloques totales\n",inet_ntoa(remote_client.sin_addr), *bloquesTotales);
					log_info(logger,"Se conectó un nuevo nodo: %s con %d bloques totales",inet_ntoa(remote_client.sin_addr), *bloquesTotales);
				} else {
					printf("Ya existe un nodo con el mismo id o direccion ip\n");
					close(newfd);
				}
			}
		} else {
			close(newfd);
			printf("Se conecto algo pero no se que fue, lo rechazo\n");
		}
	}
	//Cuando sale de este ciclo el proceso FileSystem ya se encuentra en condiciones de iniciar sus tareas

	//Este hilo va a manejar las conexiones con los nodos de forma paralela a la ejecucion del proceso
	if (pthread_create(&escucha, NULL, connection_handler_escucha, NULL) < 0) {
		perror("could not create thread");
		log_error(logger,"Falló la creación del hilo que maneja las conexiones");
		return 1;
	}

	archivos = list_create(); //Crea la lista de archivos
	directorios = list_create(); //crea la lista de directorios
	//inicializo los indices para los directorios, 0 si está libre, 1 ocupado.
	for (j = 1; j < sizeof(indiceDirectorios); j++) {
		indiceDirectorios[j] = 0;
	}
	indiceDirectorios[0] = 1; //raiz queda reservado como ocupado
	directoriosDisponibles = (MAX_DIRECTORIOS - 1);

	Menu();
	log_destroy(logger);

	return 0;
}

//Consola Menu
void DibujarMenu(void) {
	printf("################################################################\n");
	printf("# Ingrese una opción para continuar:                           #\n");
	printf("#  1) Formatear el MDFS                                        #\n");
	printf("#  2) Eliminar archivos                                        #\n");
	printf("#  3) Renombrar archivos                                       #\n");
	printf("#  4) Mover archivos                                           #\n");
	printf("#  5) Crear directorios                                        #\n");
	printf("#  6) Eliminar directorios                                     #\n");
	printf("#  7) Renombrar directorios                                    #\n");
	printf("#  8) Mover directorios                                        #\n");
	printf("#  9) Copiar un archivo local al MDFS                          #\n");
	printf("# 10) Copiar un archivo del MDFS al filesystem local           #\n");
	printf("# 11) Solicitar el MD5 de un archivo en MDFS                   #\n");
	printf("# 12) Ver los bloques que componen un archivo                  #\n");
	printf("# 13) Borrar los bloques que componen un archivo               #\n");
	printf("# 14) Copiar los bloques que componen un archivo               #\n");
	printf("# 15) Agregar un nodo de datos                                 #\n");
	printf("# 16) Eliminar un nodo de datos                                #\n");
	printf("# 17) Salir                                                    #\n");
	printf("################################################################\n");
}

int Menu(void) {
	char opchar[20];
	int opcion = 0;
	while (opcion != 17) {
		sleep(1);
		DibujarMenu();
		printf("Ingrese opción: ");
		scanf("%s", opchar);
		opcion = atoi(opchar);
		switch (opcion) {
		case 1:
			FormatearFilesystem();	break;
		case 2:
			EliminarArchivo(); break;
		case 3:
			RenombrarArchivo();	break;
		case 4:
			MoverArchivo();	break;
		case 5:
			CrearDirectorio();	break;
		case 6:
			EliminarDirectorio();	break;
		case 7:
			RenombrarDirectorio();	break;
		case 8:
			MoverDirectorio();	break;
		case 9:
			CopiarArchivoAMDFS();	break;
		case 10:
			CopiarArchivoDelMDFS();	break;
		case 11:
			MD5DeArchivo();	break;
		case 12:
			VerBloque(); break;
		case 13:
			BorrarBloque(); break;
		case 14:
			CopiarBloque(); break;
		case 15:
			AgregarNodo(); break;
		case 16:
			EliminarNodo();	break;
			//case 17: printf("Eligió Salir\n"); break;
		case 17: listar_nodos_conectados(nodos); break;
		default: printf("Opción incorrecta. Por favor ingrese una opción del 1 al 17\n"); break;
		}
	}
	return 0;
}
static t_nodo *agregar_nodo_a_lista(char nodo_id[6], int socket, int est, int est_red, char *ip, int port, int puerto_escucha, int bloques_lib,int bloques_tot) {
	t_nodo *nodo_temporal = malloc(sizeof(t_nodo));

	//===========================================================================
	//Preparo el nombre que identificara al nodo, esto antes lo hacia una funcion
	//===========================================================================
	//char nombre_temporal[10]="nodo";
	char *numero_nodo = malloc(sizeof(int));
	sprintf(numero_nodo, "%d", cantidad_nodos_historico);
	//strcat(nombre_temporal,numero_nodo);
	int i;

	//===========================================================================
	//===========================================================================
	//===========================================================================

	memset(nodo_temporal->nodo_id, '\0', 6);
	strcpy(nodo_temporal->nodo_id, nodo_id);
	nodo_temporal->socket = socket;
	//strcat(nodo_temporal->nodo_id,nombre_temporal);
	nodo_temporal->estado = est;
	nodo_temporal->estado_red = est_red;
	nodo_temporal->ip = strdup(ip);
	nodo_temporal->puerto = port;
	nodo_temporal->bloques_libres = bloques_lib;
	nodo_temporal->bloques_totales = bloques_tot;
	nodo_temporal->puerto_escucha_nodo = puerto_escucha;

	//Creo e inicializo el bitarray del nodo, 0 es bloque libre, 1 es blloque ocupado
	//Como recien se esta conectadno el nodo, todos sus bloques son libres
	for (i = 8; i < bloques_tot; i += 8)
		;
	nodo_temporal->bloques_bitarray = malloc(i / 8);
	nodo_temporal->bloques_del_nodo = bitarray_create(nodo_temporal->bloques_bitarray, i / 8);
	for (i = 0; i < nodo_temporal->bloques_totales; i++)
		bitarray_clean_bit(nodo_temporal->bloques_del_nodo, i);
	char *tmp_socket = malloc(sizeof(int));
	char *tmp_estado = malloc(sizeof(int));
	char *tmp_puerto = malloc(sizeof(int));
	char *tmp_bl_lib = malloc(sizeof(int));
	char *tmp_bl_tot = malloc(sizeof(int));
	sprintf(tmp_socket, "%d", socket);
	sprintf(tmp_estado, "%d", est);
	sprintf(tmp_puerto, "%d", port);
	sprintf(tmp_bl_lib, "%d", bloques_lib);
	sprintf(tmp_bl_tot, "%d", bloques_tot);
	//Persistencia del nodo agregado a la base de mongo
	doc = BCON_NEW("Socket", tmp_socket, "Nodo_ID", nodo_id, "Estado",tmp_estado, "IP", ip, "Puerto", tmp_puerto, "Bloques_Libres",tmp_bl_lib, "Bloques_Totales", tmp_bl_tot);
	if (!mongoc_collection_insert(collection, MONGOC_INSERT_NONE, doc, NULL,&error)) {
		printf("%s\n", error.message);
	}

	return nodo_temporal;
}

int validar_nodo_nuevo(char nodo_id[6]) {
	int i;
	t_nodo *tmp;
	for (i = 0; i < list_size(nodos); i++) {
		tmp = list_get(nodos, i);
		if (strcmp(tmp->nodo_id, nodo_id) == 0)
			return 1;
	}
	return 0;
}
int validar_nodo_reconectado(char nodo_id[6]) {
	int i;
	t_nodo *tmp;
	for (i = 0; i < list_size(nodos); i++) {
		tmp = list_get(nodos, i);
		if (strcmp(tmp->nodo_id, nodo_id) == 0)
			return 0;
	}
	return 1;
}
char *buscar_nodo_id(char *ip, int port) {
	int i;
	char *id_temporal = malloc(6);
	t_nodo *tmp;
	for (i = 0; i < list_size(nodos); i++) {
		tmp = list_get(nodos, i);
		if ((strcmp(tmp->ip, ip) == 0) && (tmp->puerto == port)) {
			strcpy(id_temporal, tmp->nodo_id);
			return id_temporal;
		}
	}
	return NULL;
}

char *obtener_md5(char *bloque) {
	int count,bak0,bak1;
		int fd[2];
		int md[2];
		int childpid;
		pipe(fd);
		pipe(md);
		char result[50];
		char *resultado_md5=malloc(32);
		memset(result,'\0',50);
		if ( (childpid = fork() ) == -1){
			fprintf(stderr, "FORK failed");
		} else if( childpid == 0) {
			bak0=dup(0);
			bak1=dup(1);
			dup2(fd[0],0);
			dup2(md[1],1);
			close(fd[1]);
			close(fd[0]);
			close(md[1]);
			close(md[0]);
			execlp("/usr/bin/md5sum","md5sum",NULL);
		}
		write(fd[1],bloque,strlen(bloque));
	    close(fd[1]);
		close(fd[0]);
		close(md[1]);
		count=read(md[0],result,36);
		close(md[0]);
		dup2(bak0,0);
		dup2(bak1,1);
		if (count>0){
			result[32]=0;
			strcpy(resultado_md5,result);
			return resultado_md5;
		}else{
			printf ("ERROR READ RESULT\n");
			exit(-1);
		}
}

void listar_nodos_conectados(t_list *nodos) {
	int i, j, cantidad_nodos;
	t_nodo *elemento;
	cantidad_nodos = list_size(nodos);
	for (i = 0; i < cantidad_nodos; i++) {
		elemento = list_get(nodos, i);
		printf("\n\n");
		printf("Nodo_ID: %s\nSocket: %d\nEstado: %d\nEstado de Conexion: %d\nIP: %s\nPuerto_Origen: %d\nPuerto_Escucha_Nodo: %d\nBloques_Libres: %d\nBloques_Totales: %d",elemento->nodo_id, elemento->socket, elemento->estado,elemento->estado_red, elemento->ip, elemento->puerto,elemento->puerto_escucha_nodo, elemento->bloques_libres,elemento->bloques_totales);
		printf("\n");
		for (j = 0; j < elemento->bloques_totales; j++)
			printf("%d", bitarray_test_bit(elemento->bloques_del_nodo, j));
	}
	printf ("\n\nBye... Bye...\n\n");
}
void *connection_handler_escucha(void) {
	int i, newfd, addrlen;
	while (1) {
		read_fds = master;
		if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
			perror("select");
			log_error(logger, "FALLO el Select");
			exit(-1);
		}
		// explorar conexiones existentes en busca de datos que leer
		for (i = 0; i <= fdmax; i++) {
			if (FD_ISSET(i, &read_fds)) { // ¡¡tenemos datos!!
				if (i == listener) {
					// gestionar nuevas conexiones, primero hay que aceptarlas
					addrlen = sizeof(struct sockaddr_in);
					if ((newfd = accept(listener,(struct sockaddr*) &remote_client,(socklen_t*) &addrlen)) == -1) {
						perror("accept");
						log_error(logger, "FALLO el ACCEPT");
						exit(-1);
					} else { //llego una nueva conexion, se acepto y ahora tengo que tratarla
						if ((read_size = recv(newfd, identificacion,sizeof(identificacion), 0)) <= 0) { //si entra aca es porque hubo un error, no considero desconexion porque es nuevo
							perror("recv");
							log_error(logger, "FALLO el Recv");
							exit(-1);
						} else {
							// el nuevo conectado me manda algo, se identifica como nodo nuevo o nodo reconectado
							// luego de que se identifique lo agregare a la lista de nodos si es nodo nuevo
							// si es nodo reconectado hay que cambiarle el estado
							if (read_size > 0 && strncmp(identificacion, "marta", 5) == 0) {
								// Se conecto el proceso Marta, le asigno un descriptor especial y lo agrego al select
								if (marta_presente == 0) {
									marta_presente = 1;
									marta_sock = newfd;
									FD_SET(newfd, &master); // añadir al conjunto maestro
									if (newfd > fdmax) { // actualizar el máximo
										fdmax = newfd;
									}
									strcpy(identificacion, "ok");
									if ((send(marta_sock, identificacion,sizeof(identificacion), 0)) == -1) {
										perror("send");
										log_error(logger, "FALLO el envio del ok a Marta");
										exit(-1);
									}
									printf("Se conectó el proceso Marta desde la ip %s\n",inet_ntoa(remote_client.sin_addr));
										log_info(logger,"Se conectó el proceso Marta desde la ip %s",inet_ntoa(remote_client.sin_addr));
								} else {
									printf("Ya existe un proceso marta conectado, no puede haber más de 1\n");
									log_warning(logger,"Ya existe un proceso marta conectado, no puede haber más de 1");
									close(newfd);
								}

							}
							if (read_size > 0 && strncmp(identificacion, "nuevo", 5) == 0) {
								bloquesTotales = malloc(sizeof(int));
								if ((read_size = recv(newfd, bloquesTotales,sizeof(int), 0)) == -1) {
									perror("recv");
									log_error(logger, "FALLO el RECV");
									exit(-1);
								}
								puerto_escucha_nodo = malloc(sizeof(int));
								if ((read_size = recv(newfd,puerto_escucha_nodo, sizeof(int), 0)) == -1) {
									perror("recv");
									log_error(logger, "FALLO el RECV");
									exit(-1);
								}
								if ((read_size = recv(newfd, nodo_id,sizeof(nodo_id), 0)) == -1) {
									perror("recv");
									log_error(logger, "FALLO el RECV");
									exit(-1);
								}
								if (read_size > 0) {
									if (validar_nodo_nuevo(nodo_id) == 0) {
										cantidad_nodos++;
										cantidad_nodos_historico = cantidad_nodos;
										FD_SET(newfd, &master); // añadir al conjunto maestro
										if (newfd > fdmax) { // actualizar el máximo
											fdmax = newfd;
										}
										list_add(nodos,agregar_nodo_a_lista(nodo_id,newfd, 0, 1,inet_ntoa(remote_client.sin_addr),remote_client.sin_port,*puerto_escucha_nodo,*bloquesTotales,*bloquesTotales));
										printf("Se conectó un nuevo nodo: %s con %d bloques totales\n",inet_ntoa(remote_client.sin_addr),*bloquesTotales);
										log_info(logger,"Se conectó un nuevo nodo: %s con %d bloques totales",inet_ntoa(remote_client.sin_addr),*bloquesTotales);
									} else {
										printf("Ya existe un nodo con el mismo id o direccion ip\n");
										close(newfd);
									}
								}
							}
							if (read_size > 0 && strncmp(identificacion, "reconectado",11) == 0) {
								if ((read_size = recv(newfd, nodo_id,sizeof(nodo_id), 0)) == -1) {
									perror("recv");
									log_error(logger, "FALLO el RECV");
									exit(-1);
								}
								if ((validar_nodo_reconectado(nodo_id)) == 0) {
									cantidad_nodos++;
									FD_SET(newfd, &master); // añadir al conjunto maestro
									if (newfd > fdmax) { // actualizar el máximo
										fdmax = newfd;
									}
									modificar_estado_nodo(nodo_id, newfd,remote_client.sin_port, 0, 1); //cambio su estado de la lista a 1 que es activo
									printf("Se reconectó el nodo %s\n",inet_ntoa(remote_client.sin_addr));
									log_info(logger, "Se reconectó el nodo %s",inet_ntoa(remote_client.sin_addr));
								} else {
									printf("Se reconecto un nodo con datos alterados, se lo desconecta\n");
									close(newfd);
								}

							}
						}
					}

					//.................................................
					//hasta aca, es el tratamiento de conexiones nuevas
					//.................................................

				} else { //si entra aca no es un cliente nuevo, es uno que ya tenia y me esta mandando algo
					// gestionar datos de un cliente
					if ((read_size = recv(i, mensaje, sizeof(mensaje), 0))
							<= 0) { //si entra aca es porque se desconecto o hubo un error
						if (read_size == 0) {
							// Un nodo o marta cerro su conexion, actualizo la lista de nodos, reviso quien fue
							if (i == marta_sock) {
								marta_presente = 0;
								close(i); // ¡Hasta luego!
								FD_CLR(i, &master); // eliminar del conjunto maestro
								printf("Marta se desconecto\n");
							} else {
								addrlen = sizeof(struct sockaddr_in);
								if ((getpeername(i,(struct sockaddr*) &remote_client,(socklen_t*) &addrlen)) == -1) {
									perror("getpeername");
									log_error(logger, "Fallo el getpeername");
									exit(-1);
								}
								char *id_temporal;
								id_temporal = buscar_nodo_id(inet_ntoa(remote_client.sin_addr),remote_client.sin_port);
								if (id_temporal != NULL) {
									strcpy(id_nodo, id_temporal);
									modificar_estado_nodo(id_nodo, i,remote_client.sin_port, 0, 0);
									printf("Se desconecto el nodo %s, %d\n",inet_ntoa(remote_client.sin_addr),remote_client.sin_port);
									close(i); // ¡Hasta luego!
									FD_CLR(i, &master); // eliminar del conjunto maestro
								} else {
									printf("ALGO SALIO MUY MAL\n");
									exit(-1);
								}
							}
						} else {
							perror("recv");
							log_error(logger, "FALLO el Recv");
							exit(-1);
						}
					} else {
						// tenemos datos de algún cliente
						// ...... Tratamiento del mensaje nuevo
					}
				}
			}
		}
	}
}

//Buscar el id del padre
uint32_t BuscarPadre(char* path) {
	t_dir* dir;
	int directorioPadre = 0,tamanio; //seteo a raíz
	if (( tamanio = list_size(directorios))==0 | string_is_empty(path) | strncmp(path,"/",1)==0){ //No hay directorios
		//printf("No se encontró el directorio\n");
		directorioPadre = -1;
		return directorioPadre;
	}
	int contadorDirectorio = 0;
	int i;
	char** directorio = string_split((char*) path, "/"); //Devuelve un array del path
	//Obtener id del padre del archivo(ante-ultima posición antes de NULL)
	while (directorio[contadorDirectorio] != NULL) {
		for (i = 0; i < tamanio; i++) { //recorro lista de directorios
			dir = list_get(directorios, i); //agarro primer directorio de la lista de directorios
			//comparo si el nombre es igual al string del array del path y el padre es igual al padre anterior
			if (((strcmp(dir->nombre, directorio[contadorDirectorio])) == 0) && (dir->padre == directorioPadre)) {
				directorioPadre = dir->id;
				contadorDirectorio++;
				break;
			} else {
				if (i == tamanio - 1) {
					//printf("No se encontró el directorio");
					directorioPadre = -1;
					return directorioPadre;
				}
			}
		}
	}
	return directorioPadre;
}

//Buscar la posición del nodo de un archivo de la lista t_archivo por el nombre del archivo y el id del padre
uint32_t BuscarArchivoPorNombre(const char *path, uint32_t idPadre) {
	unArchivo = malloc(sizeof(t_archivo));
	int i, posicionArchivo;
	char* nombreArchivo;
	int posArchivo = 0;
	int tam = list_size(archivos);
	char** directorio = string_split((char*) path, "/"); //Devuelve un array del path
	//Obtener solo el nombre del archivo(ultima posición antes de NULL)
	for (i = 0; directorio[i] != NULL; i++) {
		if (directorio[i + 1] == NULL) {
			nombreArchivo = directorio[i];
		}
	}
	for (posArchivo = 0; posArchivo < tam; posArchivo++) {
		unArchivo = list_get(archivos, posArchivo);
		if ((strcmp(unArchivo->nombre, nombreArchivo) == 0) && (unArchivo->padre == idPadre)) {
			posicionArchivo = posArchivo;
			break;
		} else {
			if (i == tam - 1) {
				printf("No se encontró el archivo");
				posicionArchivo = -1;
				exit(-1);
			}
		}
	}
	return posicionArchivo;
}

int BuscarMenorIndiceLibre(char indiceDirectorios[]) {
	//Tengo un vector donde guardo los indices para ver cuales tengo libres ya que no puedo superar 1024
	int i = 0;
	while (i < MAX_DIRECTORIOS && indiceDirectorios[i] == 1) { //Mientas sea menor a 1024 y esté ocupado, sigo buscando
		i++;
	}

	if (i < MAX_DIRECTORIOS) {
		return i; //devuelvo el menor indice libre
	} else {
		return -1; //no puedo seguir creando, me protejo de esta situación con la variable directoriosDisponibles
	}
}

void modificar_estado_nodo(char nodo_id[6], int socket, int port, int estado, int estado_red) {
	int i;
	t_nodo *tmp;
	for (i = 0; i < list_size(nodos); i++) {
		tmp = list_get(nodos, i);

		if (strcmp(tmp->nodo_id, nodo_id) == 0) {
			if (estado_red == 99) {
				tmp->estado = estado;
				break;
			} else {
				tmp->puerto = port;
				tmp->estado = estado;
				tmp->socket = socket;
				tmp->estado_red = estado_red;
				break;
			}
		}
	}
}
void formatear_nodos() {
	int i;
	for (i = 0; i < list_size(nodos); i++)
		list_remove(nodos, i);
}

void FormatearFilesystem() {
	printf("Eligió  Formatear el MDFS\n");
	if (archivos != NULL) {
		if (unArchivo->bloques != NULL) {
			list_clean(unArchivo->bloques);
		}
		list_clean(archivos);
	}
}

static void archivo_destroy(t_archivo* self) {
	free(self->nombre);
	free(self);
}

static void eliminar_bloques(t_copias *bloque) {
	free(bloque->nodo);
	free(bloque);
}

void EliminarArchivo() {
	printf("Eligió  Eliminar archivo\n");
	char* path = malloc(1);
	int i, j;
	printf("Ingrese el path del archivo \n");
	scanf("%s", path);
	uint32_t idPadre = BuscarPadre(path);
	uint32_t posArchivo = BuscarArchivoPorNombre(path, idPadre);
	unArchivo = list_get(archivos, posArchivo);
	//Eliminar bloques del archivo
	for (i = 0; i < list_size(unArchivo->bloques); i++) {
		unBloque = list_get(unArchivo->bloques, i);
		for (j = 0; i < list_size(unBloque->copias); j++) {
			list_destroy_and_destroy_elements(unBloque->copias,(void*) eliminar_bloques);
		}
	}

	//Elimnar nodo del archivo t_arhivo
	//list_remove_and_destroy_element(t_list *, int index, void(*element_destroyer)(void*));
	list_remove_and_destroy_element(archivos, posArchivo,(void*) archivo_destroy);
}

void RenombrarArchivo() {
	printf("Eligió Renombrar archivos\n");
	char* path = malloc(1);
	char* nuevoNombre = malloc(1);
	printf("Ingrese el path del archivo \n");
	scanf("%s", path);
	uint32_t idPadre = BuscarPadre(path);
	uint32_t posArchivo = BuscarArchivoPorNombre(path, idPadre);
	unArchivo = list_get(archivos, posArchivo);
	printf("Ingrese el nuevo nombre \n");
	scanf("%s", nuevoNombre);
	strcpy(unArchivo->nombre, nuevoNombre);

}

void MoverArchivo() {
	printf("Eligió Mover archivos\n");
	char* path = malloc(1);
	char* nuevoPath = malloc(1);
	printf("Ingrese el path del archivo \n");
	scanf("%s", path);
	uint32_t idPadre = BuscarPadre(path);
	uint32_t posArchivo = BuscarArchivoPorNombre(path, idPadre);
	unArchivo = list_get(archivos, posArchivo);
	printf("Ingrese el nuevo path \n");
	scanf("%s", nuevoPath);
	uint32_t idPadreNuevo = BuscarPadre(nuevoPath);
	unArchivo->padre = idPadreNuevo;
}

long ExisteEnLaLista(t_list* listaDirectorios, char* nombreDirectorioABuscar,uint32_t idPadre) {
	t_dir* elementoDeMiLista;
	elementoDeMiLista = malloc(sizeof(t_dir));
	long encontrado = -1; //trae -1 si no lo encuentra, sino trae el id del elemento
	//uso long para encontrado para cubrir el universo de uint32_t y además el -1 que necesito si no encuentro
	int tamanioLista = list_size(listaDirectorios);
	int i = 0;
	while (encontrado == -1 && i < tamanioLista) {
		elementoDeMiLista = list_get(listaDirectorios, i);
		if (strcmp(elementoDeMiLista->nombre, nombreDirectorioABuscar) == 0) { //está en mi lista un directorio con ese nombre?
			if (elementoDeMiLista->padre == idPadre) { //el que encuentro con el mismo nombre, tiene el mismo padre?
				//considero directorios con mismo nombre pero con distintos padres Ej: /utnso/tp/operativos y /utnso/operativos
				encontrado = elementoDeMiLista->id;
			}
		}
		i++;
	}
	return encontrado;
}

void CrearDirectorio() {
	//printf("Eligió Crear directorios\n");
	uint32_t idPadre;
	char* path = string_new();
	char** directorioNuevo;
	t_dir* directorioACrear;
	int cantDirACrear = 0;
	directorioACrear = malloc(sizeof(t_dir));
	long idAValidar; //uso este tipo para cubrir rango de uint32_t y el -1,  deberia mejorar el nombre de la variable
	printf("Ingrese el path del directorio desde raíz ejemplo /home/utnso \n");
	scanf("%s", path);
	directorioNuevo = string_split((char*) path, "/"); //Devuelve un array del path del directorio a crear
	//int indiceVectorDirNuevo=1;
	int indiceVectorDirNuevo = 0; //empiezo por el primero del split
	while (directorioNuevo[indiceVectorDirNuevo] != NULL) {
		//list_find(directorios,(void*) ExisteEnLaLista());  //ver más adelante de usar la función de lcommons
		if (indiceVectorDirNuevo == 0) { //el primero del split siempre va a ser hijo de raiz
			idPadre = 0;
		}
		idAValidar = ExisteEnLaLista(directorios,
				directorioNuevo[indiceVectorDirNuevo], idPadre);
		if (idAValidar != -1) {  //quiere decir que existe
			if (directorioNuevo[indiceVectorDirNuevo + 1] == NULL) {
				printf("El directorio ingresado ya existe. No se realizara ninguna accion \n");
			} else {
				idPadre = (uint32_t) idAValidar; //actualizo valor del padre con el que existe y avanzo en split para ver el siguiente directorio
			}
			indiceVectorDirNuevo++;
		} else { //hay que crear directorio
			int indiceDirectoriosNuevos;
			for (indiceDirectoriosNuevos = indiceVectorDirNuevo;directorioNuevo[indiceDirectoriosNuevos] != NULL;indiceDirectoriosNuevos++) {
				cantDirACrear++;
			}
			if (cantDirACrear <= directoriosDisponibles) { //controlo que no supere la cantidad maxima que es 1024
				while (directorioNuevo[indiceVectorDirNuevo] != NULL) {
					directorioACrear = malloc(sizeof(t_dir));
					directorioACrear->nombre = directorioNuevo[indiceVectorDirNuevo];
					directorioACrear->padre = idPadre;
					//persistir en la db: pendiente
					int id = BuscarMenorIndiceLibre(indiceDirectorios); //el nuevo id será el menor libre del vector de indices de directorios, siempre menor a 1024
					directorioACrear->id = id;
					indiceDirectorios[id] = 1; //marco como ocupado el indice correspondiente
					directoriosDisponibles--; //actualizo mi variable para saber cantidad de directorios máximos a crear
					idPadre = directorioACrear->id;
					list_add(directorios, directorioACrear);
					indiceVectorDirNuevo++;

				}
				printf("El directorio se ha creado satisfactoriamente \n");
			} else {
				printf("No se puede crear el directorio ya que sobrepasaría el límite máximo de directorios permitidos: %d\n",MAX_DIRECTORIOS);
				//No puede pasarse de 1024 directorios
			}
		}
	}
}

static void directorio_destroy(t_dir* self) {
	free(self->nombre);
	free(self);
}

void EliminarDirectorio() {
	//printf("Eligió Eliminar directorios\n");
	char* pathAEliminar = malloc(1);
	char** vectorpathAEliminar;
	t_dir* elementoDeMiListaDir;
	elementoDeMiListaDir = malloc(sizeof(t_dir));
	t_archivo* elementoDeMiListaArch;
	elementoDeMiListaArch = malloc(sizeof(t_archivo));
	int tamanioListaDir = list_size(directorios);
	int tamanioListaArch = list_size(archivos);
	int i = 0;
	uint32_t idAEliminar;
	long idEncontrado = 0;
	char encontrePos; //0 si no lo encuentra en la lista de directorios, 1 si lo encuentra
	char tieneDirOArch; //0 si no tiene, 1 si tiene subdirectorio o archivo
	int posicionElementoAEliminar;
	printf("Ingrese el path a eliminar desde raíz ejemplo /home/utnso \n");
	scanf("%s", pathAEliminar);
	vectorpathAEliminar = string_split((char*) pathAEliminar, "/");
	while (vectorpathAEliminar[i] != NULL && idEncontrado != -1) {
		if (i == 0) {
			idEncontrado = 0; //el primero que cuelga de raiz
		}
		idEncontrado = ExisteEnLaLista(directorios, vectorpathAEliminar[i],idEncontrado);
		i++;
	}
	if (idEncontrado == -1) {
		printf("No existe el directorio para eliminar \n");
	} else {
		tieneDirOArch = 0;
		idAEliminar = idEncontrado;
		i = 0;
		while (tieneDirOArch == 0 && i < tamanioListaDir) {
			elementoDeMiListaDir = list_get(directorios, i);
			if (elementoDeMiListaDir->padre == idAEliminar) { //Si tengo directorios que tengan como padre al dir que quiero eliminar
				tieneDirOArch = 1;
			}
			i++;
		}
		if (tieneDirOArch == 1) {
			printf(
					"El directorio que desea eliminar no puede ser eliminado ya que posee subdirectorios \n");
		} else {
			i = 0;
			while (tieneDirOArch == 0 && i < tamanioListaArch) {
				elementoDeMiListaArch = list_get(archivos, i);
				if (elementoDeMiListaArch->padre == idAEliminar) {
					tieneDirOArch = 1;
				}
				i++;
			}
			if (tieneDirOArch == 1) {
				printf("El directorio que desea eliminar no puede ser eliminado ya que posee archivos \n");
			} else {
				i = 0;
				encontrePos = 0; //no lo encontre
				while (encontrePos == 0 && i < tamanioListaDir) {
					elementoDeMiListaDir = list_get(directorios, i);
					if (elementoDeMiListaDir->id == idAEliminar) {
						encontrePos = 1;
					}
					i++;
				}
				posicionElementoAEliminar = i - 1;
				//list_remove_and_destroy_element(t_list *, int index, void(*element_destroyer)(void*));
				list_remove_and_destroy_element(directorios,posicionElementoAEliminar, (void*) directorio_destroy);
				indiceDirectorios[idAEliminar] = 0; //Desocupo el indice en vector de indices disponibles para poder usar ese id en el futuro
				directoriosDisponibles++; //Incremento la cantidad de directorios libres
				printf("El directorio se ha eliminado correctamente. \n");
			}
		}
	}

}

void RenombrarDirectorio() {
	//printf("Eligió Renombrar directorios\n");
	char* pathOriginal = malloc(1);
	char** vectorPathOriginal;
	char* pathNuevo = malloc(1);
	t_dir* elementoDeMiLista;
	elementoDeMiLista = malloc(sizeof(t_dir));
	int tamanioLista = list_size(directorios);
	int i = 0;
	uint32_t idARenombrar;
	long idEncontrado = 0;
	char encontrado; //0 si no lo encontro, 1 si lo encontro
	printf("Ingrese el path del original desde raíz ejemplo /home/utnso \n");
	scanf("%s", pathOriginal);
	printf("Ingrese el nuevo nombre de directorio \n");
	scanf("%s", pathNuevo);
	vectorPathOriginal = string_split((char*) pathOriginal, "/");
	while (vectorPathOriginal[i] != NULL && idEncontrado != -1) {
		if (i == 0) {
			idEncontrado = 0; //el primero que cuelga de raiz
		}
		idEncontrado = ExisteEnLaLista(directorios, vectorPathOriginal[i],idEncontrado);
		i++;
	}
	if (idEncontrado == -1) {
		printf("No existe el directorio para renombrar \n");
	} else {
		i = 0;
		encontrado = 0;
		idARenombrar = idEncontrado;
		while (encontrado == 0 && i < tamanioLista) {
			elementoDeMiLista = list_get(directorios, i);
			if (elementoDeMiLista->id == idARenombrar) {
				encontrado = 1;
				strcpy(elementoDeMiLista->nombre, pathNuevo);
			}
			i++;
		}
		printf("El directorio se ha renombrado exitosamente. \n");
	}
}

void MoverDirectorio() {
	//printf("Eligió Mover directorios\n");
	char* pathOriginal = malloc(1);
	char** vectorPathOriginal;
	char* pathNuevo = malloc(1);
	char** vectorPathNuevo;
	char* nombreDirAMover = malloc(1);
	t_dir* elementoDeMiLista;
	elementoDeMiLista = malloc(sizeof(t_dir));
	int tamanioLista = list_size(directorios);
	int i = 0;
	uint32_t idDirAMover;
	uint32_t idNuevoPadre;
	long idEncontrado = 0;
	char encontrado; //0 si no lo encontro, 1 si lo encontro
	printf("Ingrese el path original desde raíz ejemplo /home/utnso \n");
	scanf("%s", pathOriginal);
	printf("Ingrese el path del directorio al que desea moverlo desde raíz ejemplo /home/tp \n");
	scanf("%s", pathNuevo);
	vectorPathOriginal = string_split((char*) pathOriginal, "/");
	vectorPathNuevo = string_split((char*) pathNuevo, "/");
	while (vectorPathOriginal[i] != NULL && idEncontrado != -1) {
		if (i == 0) {
			idEncontrado = 0; //el primero que cuelga de raiz
		}
		idEncontrado = ExisteEnLaLista(directorios, vectorPathOriginal[i],idEncontrado);
		i++;
	}
	if (idEncontrado == -1) {
		printf("No existe el path original \n");
	} else {
		idDirAMover = idEncontrado;
		strcpy(nombreDirAMover, vectorPathOriginal[(i - 1)]); //revisar, puse -1 porque avancé hasta el NULL.
		idEncontrado = 0;
		i = 0;
		while (vectorPathNuevo[i] != NULL && idEncontrado != -1) {
			if (i == 0) {
				idEncontrado = 0; //el primero que cuelga de raiz
			}
			idEncontrado = ExisteEnLaLista(directorios, vectorPathNuevo[i],idEncontrado);
			i++;
		}
		if (idEncontrado == -1) {
			printf("No existe el path al que desea moverlo \n");
		} else {
			idNuevoPadre = idEncontrado;
			if (ExisteEnLaLista(directorios, nombreDirAMover, idNuevoPadre)	== -1) { //ver si el padre no tiene hijos que se llamen igual que el directorio a mover
				i = 0;
				encontrado = 0;
				while (encontrado == 0 && i < tamanioLista) {
					elementoDeMiLista = list_get(directorios, i);
					if (elementoDeMiLista->id == idDirAMover) {
						encontrado = 1;
						elementoDeMiLista->padre = idNuevoPadre;
					}
					i++;
				}
				printf("El directorio se ha movido satisfactoriamente \n");
			} else {
				printf("El directorio no está vacío \n");
			}
		}
	}
}

int CopiarArchivoAMDFS(){
	printf("Eligió Copiar un archivo local al MDFS\n");
    FILE * archivoLocal;
	char* path;
	char* pathMDFS;
	path = string_new();
	pathMDFS = string_new();
	int cantBytes=0;
	int pos=0;
	char car;
    memset(bufBloque,'\0',BLOCK_SIZE); //inicializo el buffer
	int j;
	printf("Ingrese el path del archivo local \n");
    scanf("%s", path);
    //Validacion de si existe el archivo en el filesystem local
    if((archivoLocal = fopen(path,"r"))==NULL){
    	log_error(logger,"El archivo que quiere copiar no existe en el filesystem local");
    	perror("fopen");
    	Menu();
    }
    printf("Ingrese el path del archivo destino \n");
    scanf("%s", pathMDFS);
    //Buscar Directorio. Si existe se muestra mensaje de error
    uint32_t idPadre = BuscarPadre(pathMDFS);
    if(idPadre == -1){
      	printf("El directorio no existe. Se debe crear el directorio desde el menú. \n");
       	Menu();
    }
    //Buscar Archivo. Si no existe se muestra mensaje de error y se debe volver al menú para crearlo
    uint32_t posArchivo = BuscarArchivoPorNombre (pathMDFS,idPadre);
    if(!(posArchivo == -1)){
     printf("El archivo ya existe. Se debe especificar un archivo nuevo. \n");
     Menu();
    }
    //Se debe crear un nuevo archivo con el nombre ingresado, cuyo padre sea "idPadre"
    while (!feof(archivoLocal)){
		car = fgetc (archivoLocal);
		cantBytes++;
		strcat(bufBloque,car); //CAR NO ES UNA CADENA, NO PODES APLICAR STRCAT
		if(car == '\n'){  //SON TIPOS DE DATOS DIFERENTES
			pos = cantBytes -1;
		}
		if(bufBloque == BLOCK_SIZE){ //BUF ES UN VECTOR, NO PODES PREGUNTAR CON ==
			if(car == '\n'){ //Caso Feliz  ---------- SON TIPOS DE DATOS DIFERENTES

			    //Ordenar los bloques del archivo según el espacio disponible
			    //Copiar el contenido del Buffer en los bloques mas vacios por triplicado
			    //Vaciar el Buffer
			    // pos = 0;
			}else{ //Caso en que el bloque no termina en "\n"
				//fseek(pos,archivo); //Retroceder hasta el "\n" anterior
				for(j=pos+1;j<BLOCK_SIZE;j++){
					strcat(bufBloque[j],"\0"); //Completar el buffer con "\0"   //CAR NO ES UNA CADENA, NO PODES APLICAR STRCAT
				}
				//Ordenar los bloques del archivo según el espacio disponible
				//Copiar el contenido del Buffer en los bloques mas vacios por triplicado
				//Vaciar el Buffer
				//pos = 0;
			}
		}
	}
    fclose(archivoLocal);
    return 0;
}


void obtenerNodosMasLibres() {
	int i, j = 0;
	t_nodo *nodoAEvaluar;

	bool nodos_mas_libres(t_nodo *vacio, t_nodo *mas_vacio) {
		return vacio->bloques_libres > mas_vacio->bloques_libres;
	}
	list_sort(nodos, (void*) nodos_mas_libres);
	for (i = 0; i < 3; i++) {
		nodoAEvaluar = list_get(nodos, i);
		if (nodoAEvaluar->estado_red == 1 && nodoAEvaluar->estado == 1
				&& nodoAEvaluar->bloques_libres > 0) {
			nodosMasLibres[i] = nodoAEvaluar;
			j++;
		}
	}
	if (j <= 3) {
		printf("No hay 3 nodos disponibles\n");
		exit(-1);
	}

}


void CopiarArchivoDelMDFS() {
	printf("Eligió Copiar un archivo del MDFS al filesystem local\n");
}

void MD5DeArchivo() {
	printf("Eligió Solicitar el MD5 de un archivo en MDFS\n");
}


void BorrarBloque() {
	//printf("Eligió Borrar un bloque que compone un archivo\n");
	int i, cantNodos, bloque, nodoEncontrado;
	nodoEncontrado = 0;
	t_nodo* nodoBuscado;
	cantNodos = list_size(nodos);
	//char* nodoId = malloc(1);
	char nodoId[6];
	printf("Ingrese el ID del nodo del que desea borrar un bloque:\n");
	scanf("%s", nodoId);
	printf("Ingrese el número de bloque que desea borrar:\n");
	scanf("%d", &bloque);
	i = 0;
	while (i < cantNodos && nodoEncontrado == 0) {
		nodoBuscado = list_get(nodos, i);
		if (strcmp(nodoBuscado->nodo_id, nodoId)==0) {
			nodoEncontrado = 1;
		}
		i++;
	}
	if (nodoEncontrado == 1){
		nodoBuscado->bloques_libres++;
		bitarray_clean_bit(nodoBuscado->bloques_del_nodo, bloque);
		printf("Se ha borrado el bloque correctamente\n");
	}
	else{
		printf("No se puede eliminar el bloque\n");
		}
}

void CopiarBloque() {
	printf("Eligió Copiar un bloque de un archivo\n");
}

void AgregarNodo(){
	//printf("Eligió Agregar un nodo de datos\n");
	int i,cantNodos, nodoEncontrado;
	nodoEncontrado =0; //0 no lo encontró, 1 lo encontró
	t_nodo* nodoAEvaluar;
	char* nodoID = malloc(1);
	cantNodos= list_size(nodos);
	for (i=0;i<cantNodos;i++){
		nodoAEvaluar = list_get(nodos,i);
		if (nodoAEvaluar->estado_red == 1 && nodoAEvaluar->estado == 0){
			printf ("\n\n");
			printf ("Nodo_ID: %s\nSocket: %d\nIP: %s\nPuerto_Origen: %d\nPuerto_Escucha_Nodo: %d\nBloques_Libres: %d\nBloques_Totales: %d", nodoAEvaluar->nodo_id, nodoAEvaluar->socket,nodoAEvaluar->ip,nodoAEvaluar->puerto,nodoAEvaluar->puerto_escucha_nodo,nodoAEvaluar->bloques_libres,nodoAEvaluar->bloques_totales);
			printf ("\n");
		}
	}
	printf("Ingrese el ID del nodo que desea agregar:\n");
	scanf("%s", nodoID);
	i = 0;
	while (i < cantNodos && nodoEncontrado == 0){
		nodoAEvaluar = list_get(nodos,i);
		if (strcmp(nodoAEvaluar->nodo_id, nodoID)==0 && nodoAEvaluar->estado == 0) {
			nodoEncontrado = 1;
		}
		i++;
	}
	if (nodoEncontrado == 1){
		modificar_estado_nodo(nodoAEvaluar->nodo_id, nodoAEvaluar->socket, nodoAEvaluar->puerto, 1, 99); //cambio su estado de la lista a 1 que es activo, invoco con 99 para solo cambiar estado
		printf("Se ha agregado el nodo %s correctamente\n",nodoID);
	}
	else{
		printf("El nodo ingresado no se puede agregar\n");
	}
}

void EliminarNodo(){
	//printf("Eligió Eliminar un nodo de datos\n");
	int i,cantNodos, nodoEncontrado;
	nodoEncontrado =0; //0 no lo encontró, 1 lo encontró
	t_nodo* nodoAEvaluar;
	char* nodoID = malloc(1);
	cantNodos= list_size(nodos);
	for (i=0;i<cantNodos;i++){
		nodoAEvaluar = list_get(nodos,i);
		if (nodoAEvaluar->estado_red == 1 && nodoAEvaluar->estado == 1){
			printf ("\n\n");
			printf ("Nodo_ID: %s\nSocket: %d\nIP: %s\nPuerto_Origen: %d\nPuerto_Escucha_Nodo: %d\nBloques_Libres: %d\nBloques_Totales: %d", nodoAEvaluar->nodo_id, nodoAEvaluar->socket,nodoAEvaluar->ip,nodoAEvaluar->puerto,nodoAEvaluar->puerto_escucha_nodo,nodoAEvaluar->bloques_libres,nodoAEvaluar->bloques_totales);
			printf ("\n");
		}
	}
	printf("Ingrese el ID del nodo que desea eliminar:\n");
	scanf("%s", nodoID);
	i = 0;
	while (i < cantNodos && nodoEncontrado == 0){
		nodoAEvaluar = list_get(nodos,i);
		if (strcmp(nodoAEvaluar->nodo_id, nodoID)==0 && nodoAEvaluar->estado == 1) {
		nodoEncontrado = 1;
	}
		i++;
	}
	if (nodoEncontrado == 1){
		modificar_estado_nodo(nodoAEvaluar->nodo_id, nodoAEvaluar->socket, nodoAEvaluar->puerto, 0, 99); //cambio su estado de la lista a 0 que es inactivo, invoco con 99 para solo cambiar estado
		printf("Se ha eliminado el nodo %s correctamente\n",nodoID);
	}
	else{
		printf("El nodo ingresado no se puede eliminar\n");
	}
}

int obtener_socket_de_nodo_con_id(char *id) {
	int i, cantidad_nodos;
	t_nodo *elemento;
	cantidad_nodos = list_size(nodos);
	for (i = 0; i <= cantidad_nodos; i++) {
		elemento = list_get(nodos, i);
		if (strcmp(elemento->nodo_id, id) == 0 && elemento->estado == 1
				&& elemento->estado_red == 1)
			return elemento->socket;
	}
	return -1;
}

void VerBloque() {
	FILE* archivoParaVerPath;
	char * bloqueParaVer;
	int nroBloque;
	bloqueParaVer = malloc(BLOCK_SIZE);
	printf("Ingrese id de Nodo ");
	scanf("%s", nodo_id);
	printf("numero de Bloque que desea ver");
	scanf("%d", nroBloque);
	enviarNumeroDeBloqueANodo(obtener_socket_de_nodo_con_id(nodo_id), nroBloque);
	bloqueParaVer = recibirBloque(obtener_socket_de_nodo_con_id(nodo_id));
	archivoParaVerPath = fopen("./archBloqueParaVer.txt", "w");
	fprintf(archivoParaVerPath, "%s", bloqueParaVer);
	printf("Se muestra path del archivo: ./archBloqueParaVer.txt");

}

void enviarNumeroDeBloqueANodo( int socket_nodo, int bloque) {
	strcpy(identificacion, "obtener bloque");
	if (send(socket_nodo, identificacion, sizeof(identificacion), 0) == -1) {
		perror("send");
		log_error(logger, "FALLO el envio del aviso de obtener bloque ");
		exit(-1);
	}
	if (send(socket_nodo, &bloque, sizeof(int), 0) == -1) {
		perror("send");
		log_error(logger, "FALLO el envio del numero de bloque");
		exit(-1);

	}
}

char *recibirBloque( socket_nodo) {
	char* bloqueAObtener;
	if ((read_size = recv(socket_nodo, identificacion, sizeof(identificacion),
			0)) <= 0) {
		perror("recv");
		log_error(logger, "FALLO el Recv");
		exit(-1);
	}
	if (strncmp(identificacion, "obtener bloque", 14) == 0) {
		bloqueAObtener = malloc(sizeof(BLOCK_SIZE));
		if (recv(socket_nodo, bloqueAObtener, sizeof(bloqueAObtener), 0) == -1) {
			perror("recv");
			log_error(logger, "FALLO el Recv");
			exit(-1);
		}
	}

	return bloqueAObtener;
}

