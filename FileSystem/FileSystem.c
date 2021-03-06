//COMMIT 700 APROBAMOSSSSSS

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
#include "FS_MDFS.h"

//Variables globales
t_list *nodos_temporales;
t_list *archivos_temporales;
t_datos_y_bloque combo;
fd_set master; // conjunto maestro de descriptores de fichero
fd_set read_fds; // conjunto temporal de descriptores de fichero para select()
t_log *logger;
t_log *log_nodos;
t_log *log_bloques_archivos;
t_log *log_capacidad_sistema;
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
int valor_persistencia=0;
int main(int argc, char *argv[]) {

	pthread_t escucha; //Hilo que va a manejar los mensajes recibidos
	int newfd;
	int addrlen;
	archivos = list_create(); //Crea la lista de archivos
	directorios = list_create(); //crea la lista de directorios
//============= REVISO LA PERSISTENCIA Y EL ESTADO DE LA ULTIMA EJECUCUION DEL FILESYSTEM ===================

	int estado_recupero = recuperar_persistencia();
	if (estado_recupero==0){
		printf ("EL Filesystem inicia en estado nuevo - no existe persistencia que recuperar \n");
	}
	if (estado_recupero==1){
		printf ("El Filesysten inicia recuperando directorios y archivos \n");
	}
	if (estado_recupero==2){
		printf ("El Filesysten inicia recuperando directorios y archivos \n");
	}
	// ================================= FIN CONTROL DE PERSISTENCIA ===================================

	int yes = 1; // para setsockopt() SO_REUSEADDR, más abajo
	configurador = config_create("resources/fsConfig.conf"); //se asigna el archivo de configuración especificado en la ruta
	logger = log_create("fsLog.log", "FileSystem", false, LOG_LEVEL_INFO);
	log_nodos = log_create("fsLog_nodos.log", "FileSystem", false, LOG_LEVEL_INFO);
	log_bloques_archivos = log_create("fsLog_bloques_archivos.log", "FileSystem", false, LOG_LEVEL_INFO);
	log_capacidad_sistema = log_create("fsLog_capacidad_sistemas.log", "FileSystem", false, LOG_LEVEL_INFO);

	FD_ZERO(&master); // borra los conjuntos maestro y temporal
	FD_ZERO(&read_fds);

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
	printf("Esperando las conexiones de los nodos iniciales\n\n");
	log_info(logger, "Esperando las conexiones de los nodos iniciales");
	while (cantidad_nodos!= config_get_int_value(configurador, "CANTIDAD_NODOS")) {
		if ((newfd = accept(listener, (struct sockaddr*) &remote_client,(socklen_t*) &addrlen)) == -1) {
			perror("accept");
			log_error(logger, "FALLO el ACCEPT");
			exit(-1);
		}
		if ((read_size = recv(newfd, identificacion, sizeof(identificacion), MSG_WAITALL)) == -1) {
			perror("recv");
			log_error(logger, "FALLO el RECV");
			exit(-1);
		}
		if (read_size > 0 && strncmp(identificacion, "nuevo", 5) == 0) {
			bloquesTotales = malloc(sizeof(int));
			//Segundo recv, aca espero recibir la capacidad del nodo
			if ((read_size = recv(newfd, bloquesTotales, sizeof(int), MSG_WAITALL))== -1) {
				perror("recv");
				log_error(logger, "FALLO el RECV");
				exit(-1);
			}
			puerto_escucha_nodo = malloc(sizeof(int));
			if ((read_size = recv(newfd, puerto_escucha_nodo, sizeof(int), MSG_WAITALL))== -1) {
				perror("recv");
				log_error(logger, "FALLO el RECV");
				exit(-1);
			}
			if ((read_size = recv(newfd, nodo_id, sizeof(nodo_id), MSG_WAITALL)) == -1) {
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
					//printf("Se conectó el %s desde %s con %d bloques disponibles\n",nodo_id,inet_ntoa(remote_client.sin_addr), *bloquesTotales);
					log_info(logger,"Se conectó el nodo: %s desde %s con %d bloques disponibles",nodo_id,inet_ntoa(remote_client.sin_addr), *bloquesTotales);
					loguear_estado_de_los_nodos(nodos);
					loguear_espacio_del_sistema(nodos);
				} else {
					//printf("Ya existe un nodo con el mismo id o direccion ip\n");
					log_info(logger,"Se conecto el %s con identidad duplicada, ya existe un nodo con ese id conectado al FileSystem\n",nodo_id);
					close(newfd);
				}
			}

			free(bloquesTotales);
			free(puerto_escucha_nodo);
		} else {
			close(newfd);
			//printf("Se conecto algo pero no se que fue, lo rechazo\n");
			log_info(logger,"Se intento conectar un proceso al FileSystem que no es un Nodo antes de que el sistema este disponible");
		}
	}
	//Cuando sale de este ciclo el proceso FileSystem ya se encuentra en condiciones de iniciar sus tareas

	//Este hilo va a manejar las conexiones con los nodos de forma paralela a la ejecucion del proceso



	if (pthread_create(&escucha, NULL, connection_handler_escucha, NULL) < 0) {
		perror("could not create thread");
		log_error(logger,"Falló la creación del hilo que maneja las conexiones");
		return 1;
	}
	Menu();
	log_destroy(logger);
	log_destroy(log_nodos);

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
	printf("# 12) Ver un bloque                                            #\n");
	printf("# 13) Borrar un bloque                                         #\n");
	printf("# 14) Copiar un bloque                                         #\n");
	printf("# 15) Agregar un nodo de datos                                 #\n");
	printf("# 16) Eliminar un nodo de datos                                #\n");
	printf("# 17) Listar archivos cargados en MDFS                         #\n");
	printf("# 18) Listar directorios creados en MDFS                       #\n");
	printf("# 19) Listar Nodos                                             #\n");
	printf("# 20) Salir                                                    #\n");
	printf("################################################################\n");
}

int Menu(void) {
	char opchar[2];
	memset(opchar, '\0', 2);
	int opcion = 0;
	while (opcion != 20) {
		sleep(1);
		opcion = 0;
		memset(opchar, '\0', 2);
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
			CopiarArchivoAMDFS(1,NULL,NULL);	break;
		case 10:
			CopiarArchivoDelMDFS(1,NULL);	break;
		case 11:
			MD5DeArchivo();	break;
		case 12:
			VerBloque(1,NULL,0); break;
		case 13:
			BorrarBloque(); break;
		case 14:
			CopiarBloque(); break;
		case 15:
			AgregarNodo(); break;
		case 16:
			EliminarNodo();	break;
		case 17:
			listar_archivos_subidos(archivos); break;
		case 18:
			listarDirectoriosCreados();break;
		case 19:
			listar_nodos_conectados(nodos); break;

			//case 20: printf("Eligió Salir\n"); break;
			//case 20: listar_nodos_conectados(nodos); break;
			//case 20: listar_archivos_subidos_usuario(archivos); break;
			//case 20: listarDirectoriosCreados();break;
			//case 20: listar_directorios(); break;

		case 20: eliminar_listas(archivos,directorios,nodos); break;  //SALIDA NORMAL, LIBERA Y NO PERSISTE

		default: printf("Opción incorrecta. Por favor ingrese una opción del 1 al 20\n"); break;
		}
	}
	return 0;
}
int recuperar_persistencia(){
	//Seccion de Directorios
	t_dir *directorio;
	FILE* dir;
	char buffer[200];
	memset(buffer,'\0',200);
	char *saveptr;
	char *nombre;
	int idYaUsado=0;
	int recupero_directorios=0;
	int recupero_archivos=0;
	dir=fopen("directorios","r");
	if (dir!=NULL){
		indiceDirectorios[idYaUsado]= 1; //reservo raíz
		directoriosDisponibles = (MAX_DIRECTORIOS - 1);
		while (fgets(buffer, sizeof(buffer),dir) != NULL){
			if (strcmp(buffer,"\n")!=0){
				recupero_directorios=1;
				directorio=malloc(sizeof(t_dir));
				directorio->id = atoi(strtok_r(buffer,";",&saveptr));
				idYaUsado = directorio->id;  //para actualizar vector de indices utilizados de vectores
				directorio->nombre=string_new();
				nombre=string_new();
				nombre = strtok_r(NULL,";",&saveptr);
				directorio->nombre=strdup(nombre);
				directorio->padre = atoi(strtok_r(NULL,";",&saveptr));
				list_add(directorios,directorio);
				//actualizo los indices ocupados de los directorios existentes en archivo de persistencia
				indiceDirectorios[idYaUsado]= 1;
				directoriosDisponibles--; //actualizo mi variable para saber cuantos me quedan para crear que no supere el limite
				memset(buffer,'\0',200);
			}
		}
		fclose(dir);
	}
	if (list_size(directorios)==0){ //No hay directorios persistidos o el archivo estaba vacio
		for (j = 1; j < sizeof(indiceDirectorios); j++) {
			indiceDirectorios[j] = 0;
		}
		indiceDirectorios[0] = 1; //raiz queda reservado como ocupado
		directoriosDisponibles = (MAX_DIRECTORIOS - 1);
	}

	//Recupero de archivos
	dir=fopen("archivos","r");
	if (dir!=NULL){
		int i,j;
		int cantidad_bloques,cantidad_copias;
		char buffer_archivo[4096];
		memset(buffer_archivo,'\0',4096);
		while (fgets(buffer_archivo, sizeof(buffer_archivo),dir) != NULL){
			if (strcmp(buffer_archivo,"\n")!=0){
				valor_persistencia=1;
				recupero_archivos=1;
				t_archivo *archivo=malloc(sizeof(t_archivo));
				nombre=string_new();
				nombre = strtok_r(buffer_archivo,";",&saveptr);
				memset(archivo->nombre,'\0',200);
				strcpy(archivo->nombre,nombre);
				archivo->padre=atoi(strtok_r(NULL,";",&saveptr));
				cantidad_bloques=atoi(strtok_r(NULL,";",&saveptr));
				archivo->bloques=list_create();
				for (i=0;i<cantidad_bloques;i++){
					t_bloque *bloque=malloc(sizeof(t_bloque));
					bloque->copias=list_create();
					cantidad_copias=atoi(strtok_r(NULL,";",&saveptr));
					for (j=0;j<cantidad_copias;j++){
						t_copias *copia=malloc(sizeof(t_copias));
						char *andre;
						andre=strtok_r(NULL,";",&saveptr);
						copia->nodo=strdup(andre);
						copia->bloqueNodo=atoi(strtok_r(NULL,";",&saveptr));
						list_add(bloque->copias,copia);
					}
					list_add(archivo->bloques,bloque);
				}
				list_add(archivos,archivo);
			}
			memset(buffer_archivo,'\0',4096);
		}
		fclose(dir);
	}
	return recupero_archivos+recupero_directorios;
}


void persistir_directorio(t_dir *directorio){
	FILE* dir;
	dir=fopen("directorios","a+");
	char *id=string_new();
	id=string_itoa((int)directorio->id);
	char *padre=string_new();
	padre=string_itoa((int)directorio->padre);
	char *nom=strdup(directorio->nombre);
	char persistir_directorio[200];
	memset(persistir_directorio,'\0',200);

	strcat(persistir_directorio,id);
	strcat(persistir_directorio,";");
	strcat(persistir_directorio,nom);
	strcat(persistir_directorio,";");
	strcat(persistir_directorio,padre);
	strcat(persistir_directorio,"\n");
	fprintf (dir,"%s",persistir_directorio);
	fclose(dir);
	free(id);
	free(padre);
	free(nom);
}

void actualizar_persistencia_directorio_eliminado(int idPadre){
	//Seccion de Directorios
	FILE* dir;
	FILE* aux;
	dir=fopen("directorios","r");
	aux=fopen("auxiliar","w");
	char buffer[200];
	char copia_buffer[200];
	memset(buffer,'\0',200);
	memset(copia_buffer,'\0',200);
	char *saveptr;
	int id;
	while (fgets(buffer, sizeof(buffer),dir) != NULL){
		if (strcmp(buffer,"\n")!=0){
			strcpy(copia_buffer,buffer);
			id = atoi(strtok_r(buffer,";",&saveptr));
			if (id!=idPadre) fprintf (aux,"%s",copia_buffer);
			memset(buffer,'\0',200);
			memset(copia_buffer,'\0',200);
		}
	}
	fclose(dir);
	fclose(aux);
	dir=fopen("directorios","w");
	aux=fopen("auxiliar","r");
	memset(buffer,'\0',200);
	while (fgets(buffer, sizeof(buffer),aux) != NULL){
		fprintf (dir,"%s",buffer);
		memset(buffer,'\0',200);
	}
	fclose(dir);
	fclose(aux);
}



void actualizar_persistencia_directorio_renombrado(int idPadre, char*nuevoNombre){
	//Seccion de Directorios
	FILE* dir;
	FILE* aux;
	dir=fopen("directorios","r");
	aux=fopen("auxiliar","w");
	char buffer[200];
	char copia_buffer[200];
	char nueva_copia[200];
	memset(nueva_copia,'\0',200);
	memset(buffer,'\0',200);
	memset(copia_buffer,'\0',200);
	char *saveptr;
	char* id=string_new();
	char* padre=string_new();
	while (fgets(buffer, sizeof(buffer),dir) != NULL){
		if (strcmp(buffer,"\n")!=0){
			strcpy(copia_buffer,buffer);
			id = strtok_r(buffer,";",&saveptr);
			padre = strtok_r(NULL,";",&saveptr);
			padre = strtok_r(NULL,";",&saveptr);
			if (atoi(id)==idPadre){
				strcat(nueva_copia,id);
				strcat(nueva_copia,";");
				strcat(nueva_copia,nuevoNombre);
				strcat(nueva_copia,";");
				strcat(nueva_copia,padre);
				strcat(nueva_copia,"\n");
				fprintf (aux,"%s",nueva_copia);
			}else fprintf (aux,"%s",copia_buffer);
			memset(buffer,'\0',200);
			memset(copia_buffer,'\0',200);
		}
	}
	fclose(dir);
	fclose(aux);
	dir=fopen("directorios","w");
	aux=fopen("auxiliar","r");
	memset(buffer,'\0',200);
	while (fgets(buffer, sizeof(buffer),aux) != NULL){
		fprintf (dir,"%s",buffer);
		memset(buffer,'\0',200);
	}
	fclose(dir);
	fclose(aux);
}



void actualizar_persistencia_directorio_movido(int idPadre, int nuevoPadre){
	//Seccion de Directorios
	FILE* dir;
	FILE* aux;
	dir=fopen("directorios","r");
	aux=fopen("auxiliar","w");
	char buffer[200];
	char copia_buffer[200];
	char nueva_copia[200];
	memset(nueva_copia,'\0',200);
	memset(buffer,'\0',200);
	memset(copia_buffer,'\0',200);
	char *saveptr;
	char* id=string_new();
	char* nombre=string_new();
	char* padre=string_new();  //este warning no se puede sacar pero esta bien, aunque dice que no hace nada hace algo
	while (fgets(buffer, sizeof(buffer),dir) != NULL){
		if (strcmp(buffer,"\n")!=0){
			strcpy(copia_buffer,buffer);
			id = strtok_r(buffer,";",&saveptr);
			nombre = strtok_r(NULL,";",&saveptr);
			padre = strtok_r(NULL,";",&saveptr);
			if (atoi(id)==idPadre){
				strcat(nueva_copia,id);
				strcat(nueva_copia,";");
				strcat(nueva_copia,nombre);
				strcat(nueva_copia,";");
				strcat(nueva_copia,string_itoa(nuevoPadre));
				strcat(nueva_copia,"\n");
				fprintf (aux,"%s",nueva_copia);
			}else fprintf (aux,"%s",copia_buffer);
			memset(buffer,'\0',200);
			memset(copia_buffer,'\0',200);
		}
	}
	fclose(dir);
	fclose(aux);
	dir=fopen("directorios","w");
	aux=fopen("auxiliar","r");
	memset(buffer,'\0',200);
	while (fgets(buffer, sizeof(buffer),aux) != NULL){
		fprintf (dir,"%s",buffer);
		memset(buffer,'\0',200);
	}
	fclose(dir);
	fclose(aux);
}
void persistir_archivo(t_archivo *archivo){
	FILE* dir;
	dir=fopen("archivos","a+");
	t_bloque *bloque;
	t_copias *copia;
	int i,j;
	fprintf (dir,"%s",archivo->nombre);
	fprintf (dir,"%s",";");
	fprintf (dir,"%d",archivo->padre);
	fprintf (dir,"%s",";");
	fprintf (dir,"%d",list_size(archivo->bloques));
	for (i=0;i<list_size(archivo->bloques);i++){
		bloque=list_get(archivo->bloques,i);
		fprintf (dir,"%s",";");
		fprintf (dir,"%d",list_size(bloque->copias));
		for (j=0;j<list_size(bloque->copias);j++){
			copia=list_get(bloque->copias,j);
			fprintf (dir,"%s",";");
			fprintf (dir,"%s",copia->nodo);
			fprintf (dir,"%s",";");
			fprintf (dir,"%d",copia->bloqueNodo);
		}
	}
	fprintf (dir,"%s","\n");
	fclose(dir);
}

void actualizar_persistencia_archivo_eliminado(char* nombre,int idPadre){
	//Seccion de Directorios
	FILE* dir;
	FILE* aux;
	dir=fopen("archivos","r");
	aux=fopen("auxiliar","w");
	char buffer[4096];
	char copia_buffer[4096];
	memset(buffer,'\0',4096);
	memset(copia_buffer,'\0',4096);
	char *saveptr;
	int padre;
	char *nombre_archivo=string_new();
	while (fgets(buffer, sizeof(buffer),dir) != NULL){
		if (strcmp(buffer,"\n")!=0){
			strcpy(copia_buffer,buffer);
			nombre_archivo = strtok_r(buffer,";",&saveptr);
			padre = atoi(strtok_r(NULL,";",&saveptr));
			if (strcmp(nombre_archivo,nombre)==0 && padre==idPadre){

			}else
				fprintf (aux,"%s",copia_buffer);
			memset(buffer,'\0',4096);
			memset(copia_buffer,'\0',4096);
		}
	}
	fclose(dir);
	fclose(aux);

	dir=fopen("archivos","w");
	aux=fopen("auxiliar","r");
	memset(buffer,'\0',4096);
	while (fgets(buffer, sizeof(buffer),aux) != NULL){
		fprintf (dir,"%s",buffer);
		memset(buffer,'\0',4096);
	}
	fclose(dir);
	fclose(aux);
}

void actualizar_persistencia_archivo_renombrado(char* nombre,int idPadre,char *nuevoNombre){
	//Seccion de Directorios
		FILE* dir;
		FILE* aux;
		dir=fopen("archivos","r");
		aux=fopen("auxiliar","w");
		char buffer[2028];
		char copia_buffer[4096];
		memset(buffer,'\0',4096);
		memset(copia_buffer,'\0',4096);
		char nueva_copia[4096];
		memset(nueva_copia,'\0',4096);
		int i,j;
		char *saveptr;
		char *padre=string_new();
		char *nombre_archivo=string_new();
		char* n_bloques=string_new();
		char *n_copias=string_new();
		char *nodo_id=string_new();
		char *n_bloque=string_new();
		while (fgets(buffer, sizeof(buffer),dir) != NULL){
			if (strcmp(buffer,"\n")!=0){
				strcpy(copia_buffer,buffer);
				nombre_archivo = strtok_r(buffer,";",&saveptr);
				padre = strtok_r(NULL,";",&saveptr);
				if (strcmp(nombre_archivo,nombre)==0 && atoi(padre)==idPadre){
					n_bloques=strtok_r(NULL,";",&saveptr);
					strcat(nueva_copia,nuevoNombre);
					strcat(nueva_copia,";");
					strcat(nueva_copia,padre);
					strcat(nueva_copia,";");
					strcat(nueva_copia,n_bloques);
					for (i=0;i<atoi(n_bloques);i++){
						n_copias=strtok_r(NULL,";",&saveptr);
						strcat(nueva_copia,";");
						strcat(nueva_copia,n_copias);
						for (j=0;j<atoi(n_copias);j++){
							nodo_id=strtok_r(NULL,";",&saveptr);
							n_bloque=strtok_r(NULL,";",&saveptr);
							strcat(nueva_copia,";");
							strcat(nueva_copia,nodo_id);
							strcat(nueva_copia,";");
							strcat(nueva_copia,n_bloque);
						}
					}
					strcat(nueva_copia,"\n");
					fprintf (aux,"%s",nueva_copia);
				}
				else
					fprintf (aux,"%s",copia_buffer);
				memset(buffer,'\0',4096);
				memset(copia_buffer,'\0',4096);
			}
		}
		fclose(dir);
		fclose(aux);

		dir=fopen("archivos","w");
		aux=fopen("auxiliar","r");
		memset(buffer,'\0',4096);
		while (fgets(buffer, sizeof(buffer),aux) != NULL){
			fprintf (dir,"%s",buffer);
			memset(buffer,'\0',4096);
		}
		fclose(dir);
		fclose(aux);
}

void actualizar_persistencia_archivo_movido(char* nombre,int idPadre,int nuevo_idPadre){
	//Seccion de Directorios
		FILE* dir;
		FILE* aux;
		dir=fopen("archivos","r");
		aux=fopen("auxiliar","w");
		char buffer[2028];
		char copia_buffer[4096];
		memset(buffer,'\0',4096);
		memset(copia_buffer,'\0',4096);
		char nueva_copia[4096];
		memset(nueva_copia,'\0',4096);
		int i,j;
		char *saveptr;
		char *padre=string_new();
		char *nombre_archivo=string_new();
		char* n_bloques=string_new();
		char *n_copias=string_new();
		char *nodo_id=string_new();
		char *n_bloque=string_new();
		while (fgets(buffer, sizeof(buffer),dir) != NULL){
			if (strcmp(buffer,"\n")!=0){
				strcpy(copia_buffer,buffer);
				nombre_archivo = strtok_r(buffer,";",&saveptr);
				padre = strtok_r(NULL,";",&saveptr);
				if (strcmp(nombre_archivo,nombre)==0 && atoi(padre)==idPadre){
					n_bloques=strtok_r(NULL,";",&saveptr);
					strcat(nueva_copia,nombre);
					strcat(nueva_copia,";");
					strcat(nueva_copia,string_itoa(nuevo_idPadre));
					strcat(nueva_copia,";");
					strcat(nueva_copia,n_bloques);
					for (i=0;i<atoi(n_bloques);i++){
						n_copias=strtok_r(NULL,";",&saveptr);
						strcat(nueva_copia,";");
						strcat(nueva_copia,n_copias);
						for (j=0;j<atoi(n_copias);j++){
							nodo_id=strtok_r(NULL,";",&saveptr);
							n_bloque=strtok_r(NULL,";",&saveptr);
							strcat(nueva_copia,";");
							strcat(nueva_copia,nodo_id);
							strcat(nueva_copia,";");
							strcat(nueva_copia,n_bloque);
						}
					}
					strcat(nueva_copia,"\n");
					fprintf (aux,"%s",nueva_copia);
				}
				else
					fprintf (aux,"%s",copia_buffer);
				memset(buffer,'\0',4096);
				memset(copia_buffer,'\0',4096);
			}
		}
		fclose(dir);
		fclose(aux);

		dir=fopen("archivos","w");
		aux=fopen("auxiliar","r");
		memset(buffer,'\0',4096);
		while (fgets(buffer, sizeof(buffer),aux) != NULL){
			fprintf (dir,"%s",buffer);
			memset(buffer,'\0',4096);
		}
		fclose(dir);
		fclose(aux);
}


void actualizar_persistencia_eliminar_bloque(char* nodoId,int bloque){
	//Seccion de Directorios
	FILE* dir;
	FILE* aux;
	dir=fopen("archivos","r");
	aux=fopen("auxiliar","w");
	char buffer[2028];
	char copia_buffer[4096];
	memset(buffer,'\0',4096);
	char buffer_2[2048];
	memset(buffer_2,'\0',2048);
	memset(copia_buffer,'\0',4096);
	char nueva_copia[4096];
	memset(nueva_copia,'\0',4096);
	int i,j;
	char *saveptr;
	int nuevo_n_copias;
	char *padre=string_new();
	char *nombre_archivo=string_new();
	char* n_bloques=string_new();
	char *n_copias=string_new();
	char *nodo_id=string_new();
	char *n_bloque=string_new();
	while (fgets(buffer, sizeof(buffer),dir) != NULL){
		if (strcmp(buffer,"\n")!=0){
			strcpy(copia_buffer,buffer);
			nombre_archivo = strtok_r(buffer,";",&saveptr);
			padre = strtok_r(NULL,";",&saveptr);
			n_bloques=strtok_r(NULL,";",&saveptr);
			strcat(nueva_copia,nombre_archivo);
			strcat(nueva_copia,";");
			strcat(nueva_copia,padre);
			strcat(nueva_copia,";");
			strcat(nueva_copia,n_bloques);
			for (i=0;i<atoi(n_bloques);i++){
				n_copias=strtok_r(NULL,";",&saveptr);
				nuevo_n_copias=atoi(n_copias);
				strcat(buffer_2,";");
				strcat(buffer_2,n_copias);
				for (j=0;j<atoi(n_copias);j++){
					nodo_id=strtok_r(NULL,";",&saveptr);
					n_bloque=strtok_r(NULL,";",&saveptr);
					if (strcmp(nodoId,nodo_id)==0 && atoi(n_bloque)==bloque){
						nuevo_n_copias--;
						buffer_2[1]=string_itoa(nuevo_n_copias)[0];
					}
					else{
						strcat(buffer_2,";");
						strcat(buffer_2,nodo_id);
						strcat(buffer_2,";");
						strcat(buffer_2,n_bloque);
					}
				}
				strcat(nueva_copia,buffer_2);
				memset(buffer_2,'\0',2048);
			}
			strcat(nueva_copia,"\n");
			fprintf (aux,"%s",nueva_copia);
			memset(nueva_copia,'\0',4096);
			memset(buffer,'\0',4096);
			memset(copia_buffer,'\0',4096);
		}
	}


	fclose(dir);
	fclose(aux);

	dir=fopen("archivos","w");
	aux=fopen("auxiliar","r");
	memset(buffer,'\0',4096);
	while (fgets(buffer, sizeof(buffer),aux) != NULL){
		fprintf (dir,"%s",buffer);
		memset(buffer,'\0',4096);
	}
	fclose(dir);
	fclose(aux);

}


void actualizar_persistencia_copiar_bloque(char* nodoId,int bloque,char* nodoId_nuevo,int bloque_nuevo){
	//Seccion de Directorios
	FILE* dir;
	FILE* aux;
	dir=fopen("archivos","r");
	aux=fopen("auxiliar","w");
	char buffer[2028];
	char copia_buffer[4096];
	memset(buffer,'\0',4096);
	char buffer_2[2048];
	memset(buffer_2,'\0',2048);
	memset(copia_buffer,'\0',4096);
	char nueva_copia[4096];
	memset(nueva_copia,'\0',4096);
	int i,j;
	char *saveptr;
	int nuevo_n_copias;
	char *padre=string_new();
	char *nombre_archivo=string_new();
	char* n_bloques=string_new();
	char *n_copias=string_new();
	char *nodo_id=string_new();
	char *n_bloque=string_new();
	while (fgets(buffer, sizeof(buffer),dir) != NULL){
		if (strcmp(buffer,"\n")!=0){
			strcpy(copia_buffer,buffer);
			nombre_archivo = strtok_r(buffer,";",&saveptr);
			padre = strtok_r(NULL,";",&saveptr);
			n_bloques=strtok_r(NULL,";",&saveptr);
			strcat(nueva_copia,nombre_archivo);
			strcat(nueva_copia,";");
			strcat(nueva_copia,padre);
			strcat(nueva_copia,";");
			strcat(nueva_copia,n_bloques);
			for (i=0;i<atoi(n_bloques);i++){
				n_copias=strtok_r(NULL,";",&saveptr);
				nuevo_n_copias=atoi(n_copias);
				strcat(buffer_2,";");
				strcat(buffer_2,n_copias);
				for (j=0;j<atoi(n_copias);j++){
					nodo_id=strtok_r(NULL,";",&saveptr);
					n_bloque=strtok_r(NULL,";",&saveptr);
					if (strcmp(nodoId,nodo_id)==0 && atoi(n_bloque)==bloque){
						nuevo_n_copias++;
						if (nuevo_n_copias==10){
							int longotud_cadena=strlen(buffer_2);
							int p;
							for (p=longotud_cadena;p>2;p--){
								buffer_2[p]=buffer_2[p-1];
							}
							buffer_2[1]=string_itoa(nuevo_n_copias)[0];
							buffer_2[2]=string_itoa(nuevo_n_copias)[1];
							strcat(buffer_2,";");
							strcat(buffer_2,nodo_id);
							strcat(buffer_2,";");
							strcat(buffer_2,n_bloque);
						}else if (nuevo_n_copias>10){
							int longotud_cadena=strlen(buffer_2);
							int p;
							for (p=longotud_cadena;p>3;p--){
								buffer_2[p]=buffer_2[p-1];
							}
							buffer_2[1]=string_itoa(nuevo_n_copias)[0];
							buffer_2[2]=string_itoa(nuevo_n_copias)[1];
							strcat(buffer_2,";");
							strcat(buffer_2,nodo_id);
							strcat(buffer_2,";");
							strcat(buffer_2,n_bloque);
						}else{
							buffer_2[1]=string_itoa(nuevo_n_copias)[0];
							strcat(buffer_2,";");
							strcat(buffer_2,nodo_id);
							strcat(buffer_2,";");
							strcat(buffer_2,n_bloque);
						}

						//Aca agrego el nuevo
						strcat(buffer_2,";");
						strcat(buffer_2,nodoId_nuevo);
						strcat(buffer_2,";");
						strcat(buffer_2,string_itoa(bloque_nuevo));

					}
					else{
						strcat(buffer_2,";");
						strcat(buffer_2,nodo_id);
						strcat(buffer_2,";");
						strcat(buffer_2,n_bloque);
					}
				}
				strcat(nueva_copia,buffer_2);
				memset(buffer_2,'\0',2048);
			}
			strcat(nueva_copia,"\n");
			fprintf (aux,"%s",nueva_copia);
			memset(nueva_copia,'\0',4096);
			memset(buffer,'\0',4096);
			memset(copia_buffer,'\0',4096);
		}
	}


	fclose(dir);
	fclose(aux);

	dir=fopen("archivos","w");
	aux=fopen("auxiliar","r");
	memset(buffer,'\0',4096);
	while (fgets(buffer, sizeof(buffer),aux) != NULL){
		fprintf (dir,"%s",buffer);
		memset(buffer,'\0',4096);
	}
	fclose(dir);
	fclose(aux);

}

void listar_directorios(){
	t_dir *dir;
	int i;
	if (list_size(directorios)==0){
		printf ("No hay directorios cargados\n");
		return;
	}
	for (i=0;i<list_size(directorios);i++){
		dir=list_get(directorios,i);
		printf ("ID: %d Nombre: %s Padre: %d\n",dir->id,dir->nombre,dir->padre);
	}
}

//Armo una lista auxiliar de subdirectorios de un directorio
t_list* obtenerHijos(int idPadre){
	t_list* listaHijos;
	listaHijos = list_create();
	t_dir* hijoAAgregar;
	t_dir* dirAEvaluar;
	int i;
	int tamanioListaDir = list_size(directorios);
	for (i = 0; i < tamanioListaDir; i++){
		dirAEvaluar=list_get(directorios,i);
		if ( dirAEvaluar->padre == idPadre){
			hijoAAgregar = malloc(sizeof(t_dir));
			hijoAAgregar->id = dirAEvaluar->id;
			hijoAAgregar->nombre = dirAEvaluar->nombre;
			hijoAAgregar->padre = dirAEvaluar->padre;
			list_add(listaHijos, hijoAAgregar);
		}
	}
	return listaHijos;
}

void listarDirectoriosCreados(){
	int cantDirectorios;
	cantDirectorios = list_size(directorios);
	char path[200];
	int i;
	int cantidadHijosDeRaiz = 0;;
	if (cantDirectorios > 0){
		t_list* listaHijosDeRaiz =list_create();
		listaHijosDeRaiz = obtenerHijos(0);
		printf ("Listado de directorios en MDFS\n\n");
		cantidadHijosDeRaiz = list_size(listaHijosDeRaiz);
		for (i = 0; i < cantidadHijosDeRaiz; i++){
			memset(path,'\0',200);
			//Seteo la / inicial de raiz
			strcpy(path,"/");
			t_dir* nodoHijo = list_get(listaHijosDeRaiz,i);
			//concateno nombre hijo
			strcat(path,nodoHijo->nombre);
			listarDirectoriosCreadosRecursiva(nodoHijo->id, path);
		}
	} else printf ("No hay directorios creados\n");
}

void listarDirectoriosCreadosRecursiva(int id, char path[200]){
	t_list* listaHijos = list_create();
	listaHijos = obtenerHijos(id);
	int cantidadHijos = list_size(listaHijos);
	int i;
	char auxPath[200];
	if(cantidadHijos == 0){
		//imprimo
		printf("%s \n" , path);
	}
	else{
		//para cada hijo vuelvo a llamar a esta funcion
		for (i = 0; i < cantidadHijos; i++){
			memset(auxPath, '\0', 200);
			//Genero String Auxiliar
			strcpy(auxPath, path);
			//obtengo directorio
			t_dir* nodoHijo = list_get(listaHijos,i);
			//concateno
			strcat(auxPath,"/");
			strcat(auxPath,nodoHijo->nombre);
			listarDirectoriosCreadosRecursiva(nodoHijo->id, auxPath);
		}
	}
}



static t_nodo *agregar_nodo_a_lista(char nodo_id[6], int socket, int est, int est_red, char *ip, int port, int puerto_escucha, int bloques_lib,int bloques_tot) {
	t_nodo *nodo_temporal = malloc(sizeof(t_nodo));
	int i;
	memset(nodo_temporal->nodo_id, '\0', 6);
	strcpy(nodo_temporal->nodo_id, nodo_id);
	nodo_temporal->socket = socket;
	nodo_temporal->estado = est;
	nodo_temporal->estado_red = est_red;
	nodo_temporal->ip = strdup(ip);
	nodo_temporal->puerto = port;
	nodo_temporal->bloques_libres = bloques_lib;
	nodo_temporal->bloques_totales = bloques_tot;
	nodo_temporal->puerto_escucha_nodo = puerto_escucha;

	//Creo e inicializo el bitarray del nodo, 0 es bloque libre, 1 es blloque ocupado
	//Como recien se esta conectadno el nodo, todos sus bloques son libres
	for (i = 8; i < bloques_tot; i += 8);
	nodo_temporal->bloques_bitarray = malloc(i / 8);
	nodo_temporal->bloques_del_nodo = bitarray_create(nodo_temporal->bloques_bitarray, i / 8);
	for (i = 0; i < nodo_temporal->bloques_totales; i++) bitarray_clean_bit(nodo_temporal->bloques_del_nodo, i);
	if (valor_persistencia==1) actualizar_nodo_persistencia(nodo_temporal);
	return nodo_temporal;
}
void actualizar_nodo_persistencia(t_nodo *nodo){
	t_archivo *archivo;
	t_bloque *bloque;
	t_copias *copia;
	int i,j,k;
	for (i=0;i<list_size(archivos);i++){
		archivo=list_get(archivos,i);
		for (j=0;j<list_size(archivo->bloques);j++){
			bloque=list_get(archivo->bloques,j);
			for (k=0;k<list_size(bloque->copias);k++){
				copia=list_get(bloque->copias,k);
				if (strcmp(copia->nodo,nodo->nodo_id)==0){
					nodo->bloques_libres--;
					bitarray_set_bit(nodo->bloques_del_nodo,copia->bloqueNodo);
				}
			}
		}
	}
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
		if ((strcmp(tmp->nodo_id, nodo_id) == 0) && (tmp->estado_red==0))
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
	free(id_temporal);
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
			free(resultado_md5);
			return NULL;
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
		printf ("\n\n");
	}
	return;
}

void loguear_estado_de_los_nodos(t_list *lista_nodos) {
	int i;
	t_nodo *nodo;
	log_info(log_nodos,"============== ESTADO DE LOS NODOS ================");
	for (i=0;i<list_size(lista_nodos);i++){
		nodo=list_get(lista_nodos,i);
		log_info(log_nodos,"Nombre %s",nodo->nodo_id);
		log_info(log_nodos,".....Bloques Totales: %d",nodo->bloques_totales);
		log_info(log_nodos,".....Bloques Libres: %d",nodo->bloques_libres);
		log_info(log_nodos,".....Estado (0 deshabilitado / 1 habilitado): %d",nodo->estado);
		log_info(log_nodos,".....Estado de red del (0 desconectado / 1 conectado): %d",nodo->estado_red);
		log_info(log_nodos,".....Direccion IP del: %s",nodo->ip);
		log_info(log_nodos,".....Puerto de Escucha: %d",nodo->puerto_escucha_nodo);
	}
}


void loguear_espacio_del_sistema(t_list *lista_nodos) {
	int i;
	t_nodo *nodo;
	int capacidad=0;
	for (i=0;i<list_size(lista_nodos);i++){
		nodo=list_get(lista_nodos,i);
		if (nodo->estado==1){
			capacidad+=nodo->bloques_libres;
		}
	}
	log_info(log_capacidad_sistema,"Capacidad actual del sistema: %dMB",capacidad*20);
}


void loguear_lista_de_bloques_de_archivo(char* nombre, uint32_t padre) {
	int i,j,k,cantidad_archivos,cantidad_bloques,cantidad_copias;
	t_archivo *elemento;
	t_bloque *bloque;
	t_copias *copia;
	cantidad_archivos = list_size(archivos);
	log_info(log_bloques_archivos,"Bloques del archivo %s",nombre);
	if (cantidad_archivos!=0){
		for (i = 0; i < cantidad_archivos; i++) {
			elemento = list_get(archivos, i);
			if (elemento->padre==padre && strcmp(elemento->nombre,nombre)==0){
				log_info(log_bloques_archivos,"\n\n");
				log_info(log_bloques_archivos,"Archivo: %s Padre: %d Bloques: %d\n",elemento->nombre,elemento->padre,list_size(elemento->bloques));
				cantidad_bloques=list_size(elemento->bloques);
				for (j = 0; j < cantidad_bloques; j++){
					bloque=list_get(elemento->bloques,j);
					log_info(log_bloques_archivos,"----- Bloque: %d\n",j);
					cantidad_copias=list_size(bloque->copias);
					for (k=0;k<cantidad_copias;k++){
						copia=list_get(bloque->copias,k);
						log_info(log_bloques_archivos,"---------- Copia %d",k);
						log_info(log_bloques_archivos," Nodo: %s Bloque: %d\n",copia->nodo,copia->bloqueNodo);
					}
				}
			}
		}
	}
}

void listar_archivos_subidos(t_list *archivos) {    //VERSION COMPLETA NO APTA PARA USUARIOS
	int i,j,k,cantidad_archivos,cantidad_bloques,cantidad_copias;
	t_archivo *elemento;
	t_bloque *bloque;
	t_copias *copia;
	cantidad_archivos = list_size(archivos);
	if (cantidad_archivos==0){
		printf ("No hay archivos cargados en MDFS\n");
		return;
	}
	for (i = 0; i < cantidad_archivos; i++) {
		elemento = list_get(archivos, i);
		printf("\n\n");
		printf("Archivo: %s\nPadre: %d\n",elemento->nombre,elemento->padre);
		printf("\n");
		cantidad_bloques=list_size(elemento->bloques);
		for (j = 0; j < cantidad_bloques; j++){
			bloque=list_get(elemento->bloques,j);
			printf ("Numero de bloque: %d\n",j);
			cantidad_copias=list_size(bloque->copias);
			for (k=0;k<cantidad_copias;k++){
				copia=list_get(bloque->copias,k);
				printf ("Copia %d del bloque %d\n",k,j);
				printf ("----------------------\n");
				printf ("	Nodo: %s\n	Bloque: %d\n\n",copia->nodo,copia->bloqueNodo);
			}
		}
	}
}


void *connection_handler_escucha(void) {
	int i, newfd, addrlen;
	char mensaje[BUF_SIZE];
	char nombreArchivoPadre[60];

	while (1) {
		read_fds = master;
		if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
			perror("select");
			log_error(logger, "FALLO el Select");
			exit(-1);
		}
		memset(mensaje,'\0',BUF_SIZE);
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
						if ((read_size = recv(newfd, identificacion,sizeof(identificacion), MSG_WAITALL)) <= 0) { //si entra aca es porque hubo un error, no considero desconexion porque es nuevo
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
									if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
										perror("send");
										log_error(logger, "FALLO el envio del ok a Marta");
										exit(-1);
									}
									//printf("\nSe conectó el proceso Marta desde la ip %s\n",inet_ntoa(remote_client.sin_addr));
									log_info(logger,"Se conectó el proceso Marta desde la ip %s",inet_ntoa(remote_client.sin_addr));



									int cant_nodos;
									cant_nodos=list_size(nodos);
									if ((send(marta_sock, &cant_nodos,sizeof(int), MSG_WAITALL)) == -1) {
										perror("send");
										log_error(logger, "FALLO el envio del ok a Marta");
										exit(-1);
									}
									for (cant_nodos=0;cant_nodos<list_size(nodos);cant_nodos++){
										t_nodo *nodo_para_marta;
										nodo_para_marta=list_get(nodos,cant_nodos);
										if ((send(marta_sock, nodo_para_marta->nodo_id,sizeof(nodo_para_marta->nodo_id), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
										if ((send(marta_sock, &nodo_para_marta->estado,sizeof(int), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
										char ip_para_enviar[17];
										memset(ip_para_enviar,'\0',17);
										strcpy(ip_para_enviar,nodo_para_marta->ip);
										if ((send(marta_sock, ip_para_enviar,sizeof(ip_para_enviar), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
										if ((send(marta_sock, &nodo_para_marta->puerto_escucha_nodo,sizeof(int), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}

									}
									int cantidad_archivos=list_size(archivos);
									if ((send(marta_sock, &cantidad_archivos,sizeof(int), MSG_WAITALL)) == -1) {
										perror("send");
										log_error(logger, "FALLO el envio del ok a Marta");
										exit(-1);
									}
									for (cantidad_archivos=0;cantidad_archivos<list_size(archivos);cantidad_archivos++){
										t_archivo *unArchivo;
										unArchivo=list_get(archivos,cantidad_archivos);
										if ((send(marta_sock, unArchivo->nombre,sizeof(unArchivo->nombre), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
										if ((send(marta_sock, &unArchivo->padre,sizeof(uint32_t), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
										int cantidad_bloques=list_size(unArchivo->bloques);
										if ((send(marta_sock, &cantidad_bloques,sizeof(int), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}

										for (cantidad_bloques=0;cantidad_bloques<list_size(unArchivo->bloques);cantidad_bloques++){
											t_bloque *unBloque;
											unBloque=list_get(unArchivo->bloques,cantidad_bloques);
											int cantidad_copias=list_size(unBloque->copias);
											if ((send(marta_sock, &cantidad_copias,sizeof(int), MSG_WAITALL)) == -1) {
												perror("send");
												log_error(logger, "FALLO el envio del ok a Marta");
												exit(-1);
											}
											for (cantidad_copias=0;cantidad_copias<list_size(unBloque->copias);cantidad_copias++){
												t_copias *unaCopia;
												unaCopia=list_get(unBloque->copias,cantidad_copias);
												char nodo_id_para_enviar[6];
												memset(nodo_id_para_enviar,'\0',6);
												strcpy(nodo_id_para_enviar,unaCopia->nodo);
												if ((send(marta_sock, nodo_id_para_enviar,sizeof(nodo_id_para_enviar), MSG_WAITALL)) == -1) {
													perror("send");
													log_error(logger, "FALLO el envio del ok a Marta");
													exit(-1);
												}
												if ((send(marta_sock, &unaCopia->bloqueNodo,sizeof(int), MSG_WAITALL)) == -1) {
													perror("send");
													log_error(logger, "FALLO el envio del ok a Marta");
													exit(-1);
												}
											}
										}
									}

								} else {
									//printf("Ya existe un proceso marta conectado, no puede haber más de 1\n");
									log_warning(logger,"Ya existe un proceso marta conectado, no puede haber más de 1");
									close(newfd);
								}

							}
							if (read_size > 0 && strncmp(identificacion, "nuevo", 5) == 0) {
								bloquesTotales = malloc(sizeof(int));
								if ((read_size = recv(newfd, bloquesTotales,sizeof(int), MSG_WAITALL)) == -1) {
									perror("recv");
									log_error(logger, "FALLO el RECV");
									exit(-1);
								}
								puerto_escucha_nodo = malloc(sizeof(int));
								if ((read_size = recv(newfd,puerto_escucha_nodo, sizeof(int), MSG_WAITALL)) == -1) {
									perror("recv");
									log_error(logger, "FALLO el RECV");
									exit(-1);
								}
								if ((read_size = recv(newfd, nodo_id,sizeof(nodo_id), MSG_WAITALL)) == -1) {
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
										//printf("\nSe conectó el %s desde %s con %d bloques disponibles\n",nodo_id,inet_ntoa(remote_client.sin_addr), *bloquesTotales);
										log_info(logger,"Se conectó el %s desde %s con %d bloques disponibles",nodo_id,inet_ntoa(remote_client.sin_addr), *bloquesTotales);
										loguear_estado_de_los_nodos(nodos);
										loguear_espacio_del_sistema(nodos);

										if(marta_presente == 1){
											char identificacion_marta[BUF_SIZE];
											memset(identificacion_marta,'\0',BUF_SIZE);
											strcpy(identificacion_marta, "nodo_nuevo");
											if ((send(marta_sock, identificacion_marta,sizeof(identificacion_marta), MSG_WAITALL)) == -1) {
												perror("send");
												log_error(logger, "FALLO el envio del ok a Marta");
												exit(-1);
											}
											if ((send(marta_sock, nodo_id,sizeof(nodo_id), MSG_WAITALL)) == -1) {
												perror("send");
												log_error(logger, "FALLO el envio del ok a Marta");
												exit(-1);
											}
											char ip_para_enviar[17];
											memset(ip_para_enviar,'\0',17);
											strcpy(ip_para_enviar,inet_ntoa(remote_client.sin_addr));
											if ((send(marta_sock, ip_para_enviar,sizeof(ip_para_enviar), MSG_WAITALL)) == -1) {
												perror("send");
												log_error(logger, "FALLO el envio del ok a Marta");
												exit(-1);
											}
											int puerto_nodo_para_marta;
											puerto_nodo_para_marta=*puerto_escucha_nodo;

											if ((send(marta_sock, &puerto_nodo_para_marta,sizeof(int), MSG_WAITALL)) == -1) {
												perror("send");
												log_error(logger, "FALLO el envio del ok a Marta");
												exit(-1);
											}
										}

									} else {
										//printf("Ya existe un nodo con el mismo id o direccion ip\n");
										log_info(logger,"Se conecto el Nodo: %s con identidad duplicada, ya existe un nodo con ese id conectado al FileSystem\n",nodo_id);
										close(newfd);
									}
								}
							}
							if (read_size > 0 && strncmp(identificacion, "reconectado",11) == 0) {
								if ((read_size = recv(newfd, nodo_id,sizeof(nodo_id), MSG_WAITALL)) == -1) {
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
									int bloques_libres_del_reconectado;
									int h;
									t_nodo *miNodo;
									for (h=0;h<list_size(nodos);h++){
										miNodo=list_get(nodos,h);
										if (strcmp(miNodo->nodo_id,nodo_id)==0) bloques_libres_del_reconectado=miNodo->bloques_libres;
									}
									//printf("Se reconectó el %s desde la ip: %s con %d bloques libres\n",nodo_id,inet_ntoa(remote_client.sin_addr),bloques_libres_del_reconectado);
									log_info(logger, "Se reconectó el %s desde la ip: %s con %d bloques libres\n",nodo_id,inet_ntoa(remote_client.sin_addr),bloques_libres_del_reconectado);
									loguear_estado_de_los_nodos(nodos);
									loguear_espacio_del_sistema(nodos);
								} else {
									//printf("Se reconecto un nodo con datos alterados, se lo desconecta\n");
									log_info(logger,"Se reconecto el Nodo: %s con identidad alterada\n",nodo_id);
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

					if (i == marta_sock) {
						if ((read_size = recv(i, mensaje, sizeof(mensaje), MSG_WAITALL)) <= 0) { //si entra aca es porque se desconecto o hubo un error
							if (read_size == 0) {
								marta_presente = 0;
								close(i); // ¡Hasta luego!
								FD_CLR(i, &master); // eliminar del conjunto maestro
								//printf("El proceso Marta se Desconecto del FileSystem\n");
								log_info(logger, "El proceso Marta se Desconecto del FileSystem\n");
							}else{
								// el recv dio -1 , osea, error
								}
							}
						if(read_size>0){
							if(strcmp(mensaje,"dame padre")==0){
								//Marta me pide el padre de un archivo (path completo)
								char** directoriosPorSeparado;
								char* directorioDestino=string_new();
								int posicionDirectorio=0;
								memset(nombreArchivoPadre,'\0',60);

								if(recv(marta_sock,nombreArchivoPadre,sizeof(nombreArchivoPadre),MSG_WAITALL)==-1){
									perror("recv");
									log_error(logger,"Fallo el envio del nombre del archivo a dar el padre por parte de Marta");
								}

								//Busco el padre
								directoriosPorSeparado=string_split(nombreArchivoPadre,"/");
								while(directoriosPorSeparado[posicionDirectorio+1]!=NULL){
									string_append(&directorioDestino,"/");
									string_append(&directorioDestino,directoriosPorSeparado[posicionDirectorio]);
									posicionDirectorio++;
								}
								//Buscar Directorio.
								uint32_t idPadre = BuscarPadre(directorioDestino);

								//Envio el padre
								if(send(marta_sock,&idPadre,sizeof(uint32_t),MSG_WAITALL)==-1){
									perror("send");
									log_error(logger,"Fallo el envio de un padre a Marta");
								}
							}
							if(strcmp(mensaje,"resultado")==0){
								//Marta me informa que un reduce termino, me va a indicar en que nodo lo voy a buscar y como se llama el archivo
								char nombreArchivoResultado[60];
								memset(nombreArchivoResultado,'\0',60);
								char nodo_resultado[6];
								memset(nodo_resultado,'\0',6);
								t_nodo *nodo_con_resultado;
								int n_nodo,corta=0;
								int bytes;
								char buff_archivo_resultado[4096];
								memset(buff_archivo_resultado,'\0',4096);
								char ruta_local[100];
								memset(ruta_local,'\0',100);
								strcpy(ruta_local,"/tmp/");
								FILE* archivo_resultado;
								if(recv(marta_sock,nodo_resultado,sizeof(nodo_resultado),MSG_WAITALL)==-1){
									perror("recv");
									log_error(logger,"Fallo el envio del nombre del archivo a dar el padre por parte de Marta");
								}
								if(recv(marta_sock,nombreArchivoResultado,sizeof(nombreArchivoResultado),MSG_WAITALL)==-1){
									perror("recv");
									log_error(logger,"Fallo el envio del nombre del archivo a dar el padre por parte de Marta");
								}
								strcat(ruta_local,nombreArchivoResultado);
								strcat(ruta_local,"-copia");
								char path_mdfs[200];
								memset(path_mdfs,'\0', 200);
								if(recv(marta_sock,path_mdfs,sizeof(path_mdfs),MSG_WAITALL)==-1){
									perror("recv");
									log_error(logger,"Fallo el envio del nombre del archivo a dar el padre por parte de Marta");
								}
								for (n_nodo=0;n_nodo<list_size(nodos);n_nodo++){
									nodo_con_resultado=list_get(nodos,n_nodo);
									if (strcmp(nodo_con_resultado->nodo_id,nodo_resultado)==0) break;
								}
								if(send(nodo_con_resultado->socket,mensaje,sizeof(mensaje),MSG_WAITALL)==-1){
									perror("send");
									log_error(logger,"Fallo el envio de un padre a Marta");
								}
								if(send(nodo_con_resultado->socket,nombreArchivoResultado,sizeof(nombreArchivoResultado),MSG_WAITALL)==-1){
									perror("send");
									log_error(logger,"Fallo el envio de un padre a Marta");
								}
								//Recibir archivo resultado del nodo y guardarlo en /tmp con el nombre del archivo
								archivo_resultado=fopen(ruta_local,"w");
								memset(buff_archivo_resultado,'\0',4096);
								int byteLeido;
								while(corta!=1){
									if((bytes=recv(nodo_con_resultado->socket,buff_archivo_resultado,4096,MSG_WAITALL))<=0){
										perror("recv");
										log_error(logger,"Fallo al recibir el resultado del nodo");
									}

									for(byteLeido=4096;byteLeido>=0;byteLeido--){
										if (buff_archivo_resultado[byteLeido-1]=='\n') break;
									}
									if(strcmp(buff_archivo_resultado,"corta")==0){
										corta=1;
										break;
									}
									fwrite(buff_archivo_resultado,sizeof(char),byteLeido,archivo_resultado);
									memset(buff_archivo_resultado,'\0',4096);
								}

								fclose(archivo_resultado);

								//printf ("Termino de recibir el archivo\n");
								//Ahora envio el archivo al mdfs
								CopiarArchivoAMDFS(99,ruta_local,path_mdfs);
								//printf ("Archivo resultado copiado exitosamente\n");
							}
						}
					}else{
						//Si no es marta, es un nodo
						if ((read_size = recv(i, mensaje, sizeof(mensaje), MSG_PEEK | MSG_DONTWAIT)) <= 0){
							if (read_size == 0) {
								//Se desconecto
								addrlen = sizeof(struct sockaddr_in);
								if ((getpeername(i,(struct sockaddr*) &remote_client,(socklen_t*) &addrlen)) == -1) {
									perror("getpeername");
									log_error(logger, "Fallo el getpeername");
									exit(-1);
								}
								char *id_temporal;
								id_temporal = buscar_nodo_id(inet_ntoa(remote_client.sin_addr),remote_client.sin_port);
								if (id_temporal != NULL) {
									strcpy(nodo_id, id_temporal);
									modificar_estado_nodo(nodo_id, i,remote_client.sin_port, 0, 0);
									//printf("Se desconecto el nodo %s, %d\n",inet_ntoa(remote_client.sin_addr),remote_client.sin_port);
									log_info(logger, "Se desconecto el Nodo: %s del FileSystem\n",nodo_id);
									loguear_estado_de_los_nodos(nodos);
									loguear_espacio_del_sistema(nodos);
									close(i); // ¡Hasta luego!
									FD_CLR(i, &master); // eliminar del conjunto maestro

									if(marta_presente == 1){
										char identificacion_marta[BUF_SIZE];
										memset(identificacion_marta,'\0',BUF_SIZE);
										strcpy(identificacion_marta, "nodo_desc");
										if ((send(marta_sock, identificacion_marta,sizeof(identificacion_marta), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
										if ((send(marta_sock, nodo_id,sizeof(nodo_id), MSG_WAITALL)) == -1) {
											perror("send");
											log_error(logger, "FALLO el envio del ok a Marta");
											exit(-1);
										}
									}



								} else {
									//printf("ALGO SALIO MUY MAL\n");
									exit(-1);
								}
							}else{
								//No voy a hacer nada aca por ahora
							}
						}
						//Aca recibiria algo de un nodo, pero no deberia
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
	if(strcmp(path,"/")==0){
		return directorioPadre;
	}
	if (( tamanio = list_size(directorios))==0 || string_is_empty(path)){ //No hay directorios
		//printf("No se encontró el directorio\n");
		directorioPadre = -1;
		return directorioPadre;
	}
	int contadorDirectorio = 0;
	int i;
	char** directorio = string_split(path, "/"); //Devuelve un array del path
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
					directorioPadre = -1;
					return directorioPadre;
				}
			}
		}
	}
	return directorioPadre;
}

//Buscar la posición del nodo de un archivo de la lista t_archivo por el nombre del archivo y el id del padre
int BuscarArchivoPorNombre(const char *path, uint32_t idPadre) {
	t_archivo* archivo;
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
	if(tam!=0){
		for (posArchivo = 0; posArchivo < tam; posArchivo++) {
			archivo = list_get(archivos, posArchivo);
			if ((strcmp(archivo->nombre, nombreArchivo) == 0) && (archivo->padre == idPadre)) {
				posicionArchivo = posArchivo;
				break;
			} else {
				if (posArchivo == tam - 1) {
					posicionArchivo = -1;
					return posicionArchivo;
				}
			}
		}
	}
	if(tam==0){
		posicionArchivo=-1;
		return posicionArchivo;
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

static void eliminar_lista_de_copias (t_copias *self){
	free(self->nodo);
	free(self);
}
static void eliminar_lista_de_nodos (t_nodo *self){
	bitarray_destroy(self->bloques_del_nodo);
	free(self->bloques_bitarray);
	free(self->ip);
	free(self);
}
static void eliminar_lista_de_bloques2(t_bloque *self){
	list_destroy_and_destroy_elements(self->copias, (void*) eliminar_lista_de_copias);
	free(self);
}

static void eliminar_lista_de_archivos2 (t_archivo *self){
	list_destroy_and_destroy_elements(self->bloques, (void*) eliminar_lista_de_bloques2);
	free(self);
}

static void eliminar_lista_de_bloques(t_bloque *self){
	list_destroy(self->copias);
	free(self);
}

static void eliminar_lista_de_archivos (t_archivo *self){
	free(self);
}
static void eliminar_lista_de_directorio(t_dir *self){
	free(self->nombre);
	free(self);
}

void FormatearFilesystem() {
	int i,k;
	printf("Eligió  Formatear el MDFS\n");
	//=====================================================================
	//======================= FORMATEO PARTE 1 ============================
	//==================ELIMINO LA LISTA DE ARCHIVOS=======================
	//=====================================================================
	list_destroy_and_destroy_elements(archivos, (void*) eliminar_lista_de_archivos2);
	archivos=list_create(); //queda la lista vacía
	//printf("Cantidad de archivos en MDFS: %d\n", list_size(archivos));
	//=====================================================================
	//======================= FORMATEO PARTE 2 ============================
	//================= VACIO LOS NODOS PARA QUEDE 0KM ====================
	//=====================================================================

	t_nodo *unNodo;
	for (i=0;i<list_size(nodos);i++){
		unNodo=list_get(nodos,i);
		unNodo->estado=0;    //PONGO EL ESTADO EN NO DISPONIBLE
		unNodo->bloques_libres=unNodo->bloques_totales;  //PONGO QUE TODOS LOS BLOQUES ESTAN DISPONIBLES
		for (k = 0; k < unNodo->bloques_totales; k++)
			bitarray_clean_bit(unNodo->bloques_del_nodo, k);   //MARCO EN TODOS LOS BITS DEL BITARRAY QUE LOS BLOQUES ESTAN DISPONIBLES
	}
	loguear_estado_de_los_nodos(nodos);
	loguear_espacio_del_sistema(nodos);

	//=====================================================================
	//======================= FORMATEO PARTE 3 ============================
	//==================ELIMINO LA LISTA DE DIRECTORIOS====================
	//=====================================================================

	list_clean_and_destroy_elements(directorios,(void*)eliminar_lista_de_directorio);

	//Actualizo vector de directorios disponibles y el control para máxima cantidad de directorios a crear
	for (j = 1; j < sizeof(indiceDirectorios); j++) {
		indiceDirectorios[j] = 0;
	}
	indiceDirectorios[0] = 1; //raiz queda reservado como ocupado
	directoriosDisponibles = (MAX_DIRECTORIOS - 1); //actualizo cantidad disponibles a crear restando raíz



	if(marta_presente == 1){
		memset(identificacion,'\0',BUF_SIZE);
		strcpy(identificacion, "marta_formatea");
		if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
			perror("send");
			log_error(logger, "FALLO el envio del ok a Marta");
			exit(-1);
		}
	}

	//Abro los archivos directorios y archivos en modo w para borrarlos
	FILE *dir;
	dir=fopen("directorios","w");
	fclose(dir);
	dir=fopen("archivos","w");
	fclose(dir);
	log_info(logger,"Se Formateo correctamente todo el FileSystem");
}

char *obtenerPath(char *nombre, int dir_id){
	char cad[200];
	char *path=string_new();
	memset(cad,'\0',200);

	if (dir_id==0){
		string_append(&path,"/");
		string_append(&path,nombre);
		return path;
	}

	t_dir *dir;
	uint32_t id_buscado=dir_id;
	int terminado=0;
	int f=0;
	char ** directoriosPorSeparado;
	int t=0;
	while (terminado!=1){
		dir=list_get(directorios,f);
		if (dir->id==id_buscado){
			if (dir->padre==0){
				strcat(cad,"/");
				strcat(cad,dir->nombre);
				terminado=1;
			}else{
				strcat(cad,"/");
				strcat(cad,dir->nombre);
				id_buscado=dir->padre;
				f=0;
			}
		}else f++;
	}
	for (f=0;f<strlen(cad);f++){
		if (cad[f]=='/') t++;
	}
	directoriosPorSeparado=string_split(cad,"/");
	for (t--;t>=0;t--){
		string_append(&path,"/");
		string_append(&path,directoriosPorSeparado[t]);
	}
	string_append(&path,"/");
	string_append(&path,nombre);
	return path;
}

void EliminarArchivo() {
	printf("Eligió  Eliminar archivo\n");
	int i;
	t_archivo* archivo;
	t_bloque* bloque;
	t_copias* copia;
	t_nodo* nodoBuscado;
	char path[200];
	memset(path,'\0',200);
	char nombre_para_marta[200];
	memset(nombre_para_marta,'\0',200);
	uint32_t padre_para_marta;
	char* directorio=string_new();
	int j,m,posicionDirectorio=0;
	char nombreArchivo[200];
	memset(nombreArchivo,'\0',200);
	char ** directoriosPorSeparado;
	if (list_size(archivos)!=0){
		printf ("Se listan los archivos existentes en MDFS:\n");
		for (i = 0; i < list_size(archivos); i++) {
			archivo = list_get(archivos, i);
			printf("\n");
			printf(".....Archivo: %s Padre: %d Bloques: %d Path: %s\n",archivo->nombre,archivo->padre,list_size(archivo->bloques),obtenerPath(archivo->nombre, archivo->padre));
			//printf(".....Archivo: %s Padre: %d Bloques: %d\n",archivo->nombre,archivo->padre,list_size(archivo->bloques));
		}
		//Si hay archivos luego de listarlos, el usuario procede
		printf("\nIngrese el path del archivo a eliminar:\n");
		scanf("%s", path);
		log_info(logger,"Se selecciono eliminar el archivo %s",path);
		int contador=0,indice_path;
		for (indice_path=0;indice_path<strlen(path);indice_path++)	if (path[indice_path]=='/') contador++;
		if (contador>1){
			directoriosPorSeparado=string_split(path,"/");
			while(directoriosPorSeparado[posicionDirectorio+1]!=NULL){
				string_append(&directorio,"/");
				string_append(&directorio,directoriosPorSeparado[posicionDirectorio]);
				posicionDirectorio++;
			}
		}else if (contador == 1){
			directorio=strdup("/");
		}else if (contador==0){
			printf ("Directorio destino mal ingresado\n");
			log_info(logger,"Fallo la eliminacion del archivo %s",path);
			return;
		}
		int idPadre = BuscarPadre(directorio);
		if (idPadre==-1){
			printf("El directorio no existe\n");
			log_info(logger,"Fallo la eliminacion del archivo %s",path);
			return;
		}
		int posArchivo = BuscarArchivoPorNombre(path, idPadre);
		if (posArchivo==-1){
			printf("El archivo no existe\n");
			log_info(logger,"Fallo la eliminacion del archivo %s",path);
			return;
		}
		archivo = list_get(archivos, posArchivo);
		loguear_lista_de_bloques_de_archivo(archivo->nombre, archivo->padre);
		strcpy(nombreArchivo,archivo->nombre);
		strcpy(nombre_para_marta,archivo->nombre);
		padre_para_marta=archivo->padre;
		for (i=0;i<list_size(archivo->bloques);i++){
			bloque=list_get(archivo->bloques,i);
			for (j=0;j<list_size(bloque->copias);j++){
				copia=list_get(bloque->copias,j);
				for (m=0;m<list_size(nodos);m++){
					nodoBuscado = list_get(nodos,m);
					if (strcmp(nodoBuscado->nodo_id, copia->nodo)==0) {
						nodoBuscado->bloques_libres++;
						bitarray_clean_bit(nodoBuscado->bloques_del_nodo, copia->bloqueNodo);
					}
				}
			}
			for (j=0;j<list_size(bloque->copias);j++){
				list_remove_and_destroy_element(bloque->copias,j,(void*)eliminar_lista_de_copias);
			}
		}
		for (i=0;i<list_size(archivo->bloques);i++){
			list_remove_and_destroy_element(archivo->bloques,i,(void*)eliminar_lista_de_bloques);
		}
		list_remove_and_destroy_element(archivos,posArchivo,(void*)eliminar_lista_de_archivos);
		printf ("Archivo eliminado exitosamente\n");
		log_info(logger,"Se elimino correctamente el archivo %s",path);
		loguear_estado_de_los_nodos(nodos);
		loguear_espacio_del_sistema(nodos);
		actualizar_persistencia_archivo_eliminado(nombreArchivo,idPadre);
		//Actualizo a Marta
		if(marta_presente == 1){
			memset(identificacion,'\0',BUF_SIZE);
			strcpy(identificacion, "elim_arch");
			if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, nombre_para_marta,sizeof(nombre_para_marta), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, &padre_para_marta,sizeof(padre_para_marta), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
		}
	}else printf ("No hay archivos cargados en MDFS\n");

}
void RenombrarArchivo() {
	printf("Eligió Renombrar archivos\n");
	if (list_size(archivos)!=0){
		t_archivo* archivo;
		char ** directoriosPorSeparado;
		char* directorio=string_new();
		int posicionDirectorio=0;
		char path[200];
		memset(path,'\0',200);
		char nuevoNombre[200];
		memset(nuevoNombre,'\0',200);
		char viejoNombre[200];
		memset(viejoNombre,'\0',200);
		int i;
		printf ("Se listan los archivos existentes en MDFS:\n");
		for (i = 0; i < list_size(archivos); i++) {
			archivo = list_get(archivos, i);
			printf("\n");
			printf(".....Archivo: %s Padre: %d Bloques: %d Path: %s\n",archivo->nombre,archivo->padre,list_size(archivo->bloques),obtenerPath(archivo->nombre, archivo->padre));
			//printf(".....Archivo: %s Padre: %d Bloques: %d\n",archivo->nombre,archivo->padre,list_size(archivo->bloques));
		}
		printf("\nIngrese el path del archivo a renombrar, por ejemplo /home/tp/nombreArchivo \n");
		scanf("%s", path);
		log_info(logger,"Selecciono renombrar el archivo: %s",path);
		int contador=0,indice_path;
		for (indice_path=0;indice_path<strlen(path);indice_path++)	if (path[indice_path]=='/') contador++;
		if (contador>1){
			directoriosPorSeparado=string_split(path,"/");
			while(directoriosPorSeparado[posicionDirectorio+1]!=NULL){
				string_append(&directorio,"/");
				string_append(&directorio,directoriosPorSeparado[posicionDirectorio]);
				posicionDirectorio++;
			}
		}else if (contador == 1){
			directorio=strdup("/");
		}else if (contador==0){
			printf ("Directorio destino mal ingresado\n");
			log_info(logger,"Fallo la renombracion del archivo %s",path);
			return;
		}

		int idPadre = BuscarPadre(directorio);
		if (idPadre==-1){
			printf("El archivo no existe\n");
			log_info(logger,"Fallo la renombracion del archivo %s",path);
			return;
		}
		int posArchivo = BuscarArchivoPorNombre(path, idPadre);
		archivo = list_get(archivos, posArchivo);
		loguear_lista_de_bloques_de_archivo(archivo->nombre, archivo->padre);
		printf("Ingrese el nuevo nombre \n");
		scanf("%s", nuevoNombre);
		strcpy(viejoNombre,archivo->nombre);
		strcpy(archivo->nombre, nuevoNombre);
		printf ("Archivo renombrado exitosamente\n");
		log_info(logger,"El archivo %s se renombro exitosamente a ",path,nuevoNombre);
		actualizar_persistencia_archivo_renombrado(viejoNombre,idPadre,nuevoNombre);

		//Aviso a marta que tiene que renombrar un archivo

		if(marta_presente == 1){
			memset(identificacion,'\0',BUF_SIZE);
			strcpy(identificacion, "renom_arch");
			if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, viejoNombre,sizeof(viejoNombre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, &idPadre,sizeof(idPadre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, nuevoNombre,sizeof(nuevoNombre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
		}
	}else printf ("No hay archivos cargados en MDFS\n");


}
void MoverArchivo() {
	printf("Eligió Mover archivos\n");
	int i;
	t_archivo* archivo;
	if (list_size(archivos)!=0){
		printf ("Se listan los archivos existentes en MDFS:\n");
		for (i = 0; i < list_size(archivos); i++) {
			archivo = list_get(archivos, i);
			printf("\n");
			printf(".....Archivo: %s Padre: %d Bloques: %d Path: %s\n",archivo->nombre,archivo->padre,list_size(archivo->bloques),obtenerPath(archivo->nombre, archivo->padre));
			//printf(".....Archivo: %s Padre: %d Bloques: %d\n",archivo->nombre,archivo->padre,list_size(archivo->bloques));
		}
		char path[200];
		char nuevoPath[200];
		char **directoriosPorSeparado;
		int posicionDirectorio=0;
		char *directorioDestino=string_new();
		printf("\nIngrese el path del archivo que quiere mover, por ejemplo /home/tp/nombreArchivo \n");
		memset(path,'\0',200);
		scanf("%s", path);
		log_info(logger,"Selecciono mover el archivo %s",path);
		int contador=0,indice_path;
		for (indice_path=0;indice_path<strlen(path);indice_path++)	if (path[indice_path]=='/') contador++;
		if (contador>1){
			directoriosPorSeparado=string_split(path,"/");
			while(directoriosPorSeparado[posicionDirectorio+1]!=NULL){
				string_append(&directorioDestino,"/");
				string_append(&directorioDestino,directoriosPorSeparado[posicionDirectorio]);
				posicionDirectorio++;
			}
		}else if (contador == 1){
			directorioDestino=strdup("/");
		}else if (contador==0){
			printf ("Directorio destino mal ingresado\n");
			log_info(logger,"Fallo mover el archivo %s",path);
			return;
		}

		int idPadre = BuscarPadre(directorioDestino);
		printf ("Padre: %d\n",idPadre);
		if (idPadre==-1){
			printf ("El path no existe\n");
			log_info(logger,"Fallo mover el archivo %s",path);
			return;
		}
		int posArchivo = BuscarArchivoPorNombre(path, idPadre);
		if (posArchivo==-1){
			printf ("El archivo no existe\n");
			log_info(logger,"Fallo mover el archivo %s",path);
			return;
		}
		archivo = list_get(archivos, posArchivo);
		loguear_lista_de_bloques_de_archivo(archivo->nombre, archivo->padre);
		listarDirectoriosCreados();
		printf("Ingrese el nuevo path \n");
		memset(nuevoPath,'\0',200);
		scanf("%s", nuevoPath);
		log_info(logger,"El nuevo path del archivo sera %s",nuevoPath);
		int idPadreNuevo = BuscarPadre(nuevoPath);
		if (idPadreNuevo==-1){
			printf ("El path destino no existe\n");
			log_info(logger,"Fallo mover el archivo a %s",nuevoPath);
			return;
		}
		archivo->padre = idPadreNuevo;
		actualizar_persistencia_archivo_movido(archivo->nombre,idPadre,idPadreNuevo);
		printf ("Archivo movido exitosamente\n");
		log_info(logger,"El archivo %s se movio correctamente a %s",path,nuevoPath);
		if(marta_presente == 1){
			memset(identificacion,'\0',BUF_SIZE);
			strcpy(identificacion, "mov_arch");
			if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, archivo->nombre,sizeof(archivo->nombre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, &idPadre,sizeof(idPadre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, &idPadreNuevo,sizeof(idPadreNuevo), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
		}
	}
}

long ExisteEnLaLista(t_list* listaDirectorios, char* nombreDirectorioABuscar,uint32_t idPadre) {
	t_dir* elementoDeMiLista;
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
	char path[200];
	char** directorioNuevo;
	t_dir* directorioACrear;
	int cantDirACrear = 0;
	long idAValidar; //uso este tipo para cubrir rango de uint32_t y el -1,  deberia mejorar el nombre de la variable
	printf("Ingrese el path del directorio desde raíz ejemplo /home/utnso \n");
	memset(path,'\0',200);
	scanf("%s", path);
	log_info(logger,"Selecciono crear el directorio %s",path);
	if (strcmp(path,"/")==0){
		printf("No puede crearlo porque ese directorio ya es raíz \n");
		log_info(logger,"Fallo crear el directorio %s",path);
		return;
	}
	directorioNuevo = string_split((char*) path, "/"); //Devuelve un array del path del directorio a crear
	int indiceVectorDirNuevo = 0; //empiezo por el primero del split
	while (directorioNuevo[indiceVectorDirNuevo] != NULL) {
		if (indiceVectorDirNuevo == 0) { //el primero del split siempre va a ser hijo de raiz
			idPadre = 0;
		}
		idAValidar = ExisteEnLaLista(directorios,
				directorioNuevo[indiceVectorDirNuevo], idPadre);
		if (idAValidar != -1) {  //quiere decir que existe
			if (directorioNuevo[indiceVectorDirNuevo + 1] == NULL) {
				printf("El directorio ingresado ya existe. No se realizara ninguna accion \n");
				log_info(logger,"Fallo crear el directorio %s, ya existe",path);
				listarDirectoriosCreados();
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
					persistir_directorio(directorioACrear);

				}
				printf("El directorio se ha creado satisfactoriamente \n");
				log_info(logger,"El directorio %s se creo correctamente",path);
				listarDirectoriosCreados();
			} else {
				printf("No se puede crear el directorio ya que sobrepasaría el límite máximo de directorios permitidos: %d\n",MAX_DIRECTORIOS);
				log_info(logger,"Fallo crear el directorio %s, max. cantidad de directorios superada",path);
				//No puede pasarse de 1024 directorios
				listarDirectoriosCreados();
			}
		}
	}
}

static void directorio_destroy(t_dir* self) {
	free(self->nombre);
	free(self);
}

void eliminar_listas(t_list *archivos_l, t_list *directorios_l, t_list *nodos_l){
	FILE* dir;

	//=====================================================================
	//======================= LIBERAR PARTE 1 =============================
	//==================ELIMINO LA LISTA DE ARCHIVOS=======================
	//=====================================================================
	if (archivos_l!=NULL){
		/*t_archivo *archi;
		t_bloque *bloq;

		for (i=0;i<list_size(archivos_l);i++){
			archi=list_get(archivos_l,i);
			for (j=0;j<list_size(archi->bloques);j++){
				bloq=list_get(archi->bloques,j);
				for (k=0;k<list_size(bloq->copias);k++){
					list_remove_and_destroy_element(bloq->copias,k,(void*)eliminar_lista_de_copias);
				}
				list_remove_and_destroy_element(archi->bloques,j,(void*)eliminar_lista_de_bloques);
			}
			list_remove_and_destroy_element(archivos_l,i,(void*)eliminar_lista_de_archivos);
		}
		list_destroy(archivos_l);*/
		list_destroy_and_destroy_elements(archivos_l, (void*) eliminar_lista_de_archivos2);

	}
	//=====================================================================
	//======================= LIBERAR PARTE 2 =============================
	//====================== LIBERO LOS NODOS =============================
	//=====================================================================

	if (nodos_l!=NULL)
		list_destroy_and_destroy_elements(nodos_l,(void*)eliminar_lista_de_nodos);

	//=====================================================================
	//======================= FORMATEO PARTE 3 ============================
	//==================ELIMINO LA LISTA DE DIRECTORIOS====================
	//=====================================================================

	if (directorios_l!=NULL){
		list_destroy_and_destroy_elements(directorios_l,(void*)eliminar_lista_de_directorio);
	}
	//Salida corresta del FS
	if (archivos_l!=NULL && directorios_l!=NULL && nodos_l!=NULL){
		printf ("Adios!\n");
		log_info(logger,"Se sale correctamente del Filesystem y se borra toda persistencia");

		//Abro los archivos directorios y archivos en modo w para borrarlos
		dir=fopen("directorios","w");
		fclose(dir);
		dir=fopen("archivos","w");
		fclose(dir);
		exit(0);
	}

}

void EliminarDirectorio() {
	//printf("Eligió Eliminar directorios\n");
	listarDirectoriosCreados();
	int tamanioListaDir = list_size(directorios);
	if (tamanioListaDir > 0){

		char pathAEliminar[200];
		memset(pathAEliminar, '\0', 200);
		char copia_pathAEliminar[200];
		memset(copia_pathAEliminar, '\0', 200);
		char** vectorpathAEliminar;
		t_dir* elementoDeMiListaDir;
		t_archivo* elementoDeMiListaArch;
		int tamanioListaArch = list_size(archivos);
		int i = 0;
		uint32_t idAEliminar;
		long idEncontrado = 0;
		char encontrePos; //0 si no lo encuentra en la lista de directorios, 1 si lo encuentra
		char tieneDirOArch; //0 si no tiene, 1 si tiene subdirectorio o archivo
		int posicionElementoAEliminar;
		printf("Ingrese el path del directorio que desea eliminar, desde raíz ejemplo /home/utnso \n");
		scanf("%s", pathAEliminar);
		log_info(logger,"Selecciono eliminar el directorio %s",pathAEliminar);
		if (strcmp(pathAEliminar,"/")==0){
			printf("No se puede puede eliminar la raíz \n");
			log_info(logger,"Fallo eliminar el directorio %s",pathAEliminar);
			return;
		}
		strcpy(copia_pathAEliminar,pathAEliminar);
		int idPadre = BuscarPadre(copia_pathAEliminar);
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
			log_info(logger,"Fallo eliminar el directorio %s",pathAEliminar);
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
						log_info(logger,"Fallo eliminar el directorio %s",pathAEliminar);
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
					log_info(logger,"Fallo eliminar el directorio %s",pathAEliminar);
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
					list_remove_and_destroy_element(directorios,posicionElementoAEliminar, (void*) directorio_destroy);
					indiceDirectorios[idAEliminar] = 0; //Desocupo el indice en vector de indices disponibles para poder usar ese id en el futuro
					directoriosDisponibles++; //Incremento la cantidad de directorios libres
					actualizar_persistencia_directorio_eliminado(idPadre);
					printf("El directorio se ha eliminado correctamente. \n");
					log_info(logger,"El directorio %s se elimino correctamente",pathAEliminar);
				}
			}
		}
	} else {
		return;
	}
}

void RenombrarDirectorio() {
	//printf("Eligió Renombrar directorios\n");
	listarDirectoriosCreados();
	int tamanioLista = list_size(directorios);
	if (tamanioLista > 0){
		char pathOriginal[200];
		memset(pathOriginal, '\0', 200);
		char copiaPath[200];
		memset(copiaPath, '\0', 200);
		char** vectorPathOriginal;
		char pathNuevo[200];
		memset(pathNuevo, '\0', 200);
		t_dir* elementoDeMiLista;
		int i = 0;
		int idParaRenombrar;
		uint32_t idARenombrar;
		long idEncontrado = 0;
		char encontrado; //0 si no lo encontro, 1 si lo encontro
		printf("Ingrese el path del directorio que desea renombrar, desde raíz ejemplo /home/utnso \n");
		scanf("%s", pathOriginal);
		log_info(logger,"Selecciono renombrar el directorio %s",pathOriginal);
		strcpy(copiaPath,pathOriginal);
		idParaRenombrar=BuscarPadre(copiaPath);
		printf("Ingrese el nuevo nombre de directorio sin barras \n");
		scanf("%s", pathNuevo);
		log_info(logger,"Selecciono renombrar el directorio %s por %s",pathOriginal,pathNuevo);
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
			log_info(logger,"Fallo renombrar el directorio %s",pathOriginal);
		} else {
			i = 0;
			encontrado = 0;
			idARenombrar = idEncontrado;
			while (encontrado == 0 && i < tamanioLista) {
				elementoDeMiLista = list_get(directorios, i);
				if (elementoDeMiLista->id == idARenombrar) {
					encontrado = 1;
					elementoDeMiLista->nombre=strdup(pathNuevo);
				}
				i++;
			}
			printf("El directorio se ha renombrado exitosamente. \n");
			log_info(logger,"El directorio %s fue renombrado correctamente por %s",pathOriginal,pathNuevo);
			actualizar_persistencia_directorio_renombrado(idParaRenombrar,pathNuevo);
			listarDirectoriosCreados();
		}
	} else {
		return;
	}
}

void MoverDirectorio() {
	//printf("Eligió Mover directorios\n");
	listarDirectoriosCreados();
	int tamanioLista = list_size(directorios);
	if (tamanioLista > 0){
		char pathOriginal[200];
		memset(pathOriginal, '\0', 200);
		char** vectorPathOriginal;
		char pathNuevo[200];
		memset(pathNuevo, '\0', 200);
		char** vectorPathNuevo;
		char nombreDirAMover[200];
		memset(nombreDirAMover, '\0', 200);
		t_dir* elementoDeMiLista;
		int i = 0;
		uint32_t idDirAMover;
		uint32_t idNuevoPadre;
		long idEncontrado = 0;
		int padreViejo;
		int padreNuevo;
		char encontrado; //0 si no lo encontro, 1 si lo encontro
		printf("Ingrese el path del directorio que desea mover, desde raíz ejemplo /home/utnso \n");
		scanf("%s", pathOriginal);
		padreViejo = BuscarPadre(pathOriginal);
		printf("Ingrese el path del directorio al que desea moverlo, desde raíz ejemplo /home/tp \n");
		scanf("%s", pathNuevo);
		log_info(logger,"Selecciono mover el directorio %s a %s",pathOriginal,pathNuevo);
		if (strcmp(pathNuevo,"/")==0){
			padreNuevo = 0;
		}
		else{
			padreNuevo = BuscarPadre(pathNuevo);
			vectorPathNuevo = string_split((char*) pathNuevo, "/");
		}
		vectorPathOriginal = string_split((char*) pathOriginal, "/");
		while (vectorPathOriginal[i] != NULL && idEncontrado != -1) {
			if (i == 0) {
				idEncontrado = 0; //el primero que cuelga de raiz
			}
			idEncontrado = ExisteEnLaLista(directorios, vectorPathOriginal[i],idEncontrado);
			i++;
		}
		if (idEncontrado == -1) {
			printf("No existe el path original \n");
			log_info(logger,"Fallo mover el directorio %s",pathOriginal);
		} else {
			idDirAMover = idEncontrado;
			strcpy(nombreDirAMover, vectorPathOriginal[(i - 1)]); //revisar, puse -1 porque avancé hasta el NULL.
			idEncontrado = 0;
			i = 0;
			if (padreNuevo == 0){
				idEncontrado = 0;
			}
			else{
				while (vectorPathNuevo[i] != NULL && idEncontrado != -1) {
					if (i == 0) {
						idEncontrado = 0; //el primero que cuelga de raiz
					}
					idEncontrado = ExisteEnLaLista(directorios, vectorPathNuevo[i],idEncontrado);
					i++;
				}
			}
			if (idEncontrado == -1) {
				printf("No existe el path al que desea moverlo \n");
				log_info(logger,"Fallo mover el directorio %s",pathOriginal);
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
					log_info(logger,"El directorio %s se movio correctamente a %s",pathOriginal,pathNuevo);
					actualizar_persistencia_directorio_movido(padreViejo, padreNuevo);
					listarDirectoriosCreados();
				} else {
					printf("El directorio no está vacío \n");
				}
			}
		}
	}
	else {
		return;
	}
}

bool nodos_mas_libres(t_nodo *vacio, t_nodo *mas_vacio) {
	return vacio->bloques_libres > mas_vacio->bloques_libres;
}

int copiar_lista_de_archivos(t_list* destino, t_list* origen){
	int i,j,k;
	for (i=0;i<list_size(origen);i++){
		t_archivo *original;
		t_archivo *copia=malloc(sizeof(t_archivo));
		original=list_get(origen,i);
		copia->bloques=original->bloques;
		strcpy(copia->nombre,original->nombre);
		copia->padre=original->padre;
		copia->bloques=list_create();
		for (j=0;j<list_size(original->bloques);j++){
			t_bloque *bloque_original;
			t_bloque *bloque_copia=malloc(sizeof(t_bloque));
			bloque_original=list_get(original->bloques,j);
			bloque_copia->copias=list_create();
			for(k=0;k<list_size(bloque_original->copias);k++){
				t_copias *copia_original;
				t_copias *copia_copia=malloc(sizeof(t_copias));
				copia_original=list_get(bloque_original->copias,k);
				copia_copia->bloqueNodo=copia_original->bloqueNodo;
				copia_copia->nodo=strdup(copia_original->nodo);
				list_add(bloque_copia->copias,copia_copia);
			}
			list_add(copia->bloques,bloque_copia);
		}
		list_add(destino,copia);
	}
	return 0;
}
int copiar_lista_de_nodos(t_list* destino, t_list* origen){
	int i,k;
	for (i=0;i<list_size(origen);i++){
		t_nodo *original;
		t_nodo *copia=malloc(sizeof(t_nodo));
		original=list_get(origen,i);

		memset(copia->nodo_id, '\0', 6);
		strcpy(copia->nodo_id, original->nodo_id);
		copia->socket = original->socket;
		copia->estado = original->estado;
		copia->estado_red = original->estado_red;
		copia->ip = strdup(original->ip);
		copia->puerto = original->puerto;
		copia->bloques_libres = original->bloques_libres;
		copia->bloques_totales = original->bloques_totales;
		copia->puerto_escucha_nodo = original->puerto_escucha_nodo;

		//Creo e inicializo el bitarray del nodo, 0 es bloque libre, 1 es blloque ocupado
		for (k = 8; k < original->bloques_totales; k += 8);
		copia->bloques_bitarray = malloc(k / 8);
		copia->bloques_del_nodo = bitarray_create(copia->bloques_bitarray, k / 8);
		for (k = 0; k < copia->bloques_totales; k++)
			if (!bitarray_test_bit(original->bloques_del_nodo, k))
				bitarray_clean_bit(copia->bloques_del_nodo, k);
			else bitarray_set_bit(copia->bloques_del_nodo, k);
		list_add(destino,copia);

	}
	return 0;
}

int CopiarArchivoAMDFS(int flag, char* archvo_local, char* archivo_mdfs){

	FILE * archivoLocal;
    char* directorioDestino=string_new();
    char ** directoriosPorSeparado;
    int posicionDirectorio=0;
    char path[200];
    char pathMDFS[200];
    memset(path,'\0',200);
    memset(pathMDFS,'\0',200);
    nodos_temporales=list_create();
	if (copiar_lista_de_nodos(nodos_temporales,nodos)){
	 	printf ("No se pudo crear la copia de la lista de nodos\n");
	 	return -1;
	}
    t_nodo *nodo_temporal;
    t_archivo *archivo_temporal;

    archivos_temporales=list_create();
    if (copiar_lista_de_archivos(archivos_temporales,archivos)){
    	printf ("No se pudo crear la copia de la lista de nodos\n");
    	return -1;
    }

    char handshake[15]="copiar_archivo";
	char ruta[200];
	memset(ruta,'\0',200);
	int indice=0;
	uint32_t cantBytes=0;
	int pos=0;
	int total_enviado;
	int corte=0;
	int j;


	if (flag!=99){
		printf("Eligió Copiar un archivo local al MDFS\n");
		printf("Ingrese el path del archivo local desde raíz, por ejemplo /home/tp/nombreArchivo \n");
		scanf("%s", path);
		log_info(logger,"Selecciono copiar el archivo %s al MDFS",path);
	}else strcpy(path,archvo_local);

	//Validacion de si existe el archivo en el filesystem local
	if((archivoLocal = fopen(path,"r"))==NULL){
    	log_error(logger,"El archivo que quiere copiar no existe en el filesystem local");
    	perror("fopen");
    	return -1;
    }


	if (flag!=99){
		listarDirectoriosCreados();
		printf("Ingrese el path del archivo destino desde raíz, por ejemplo /tmp/nombreArchivo \n");
		scanf("%s", pathMDFS);
		log_info(logger,"El destino seleccionado en MDFS es: %s",pathMDFS);
	}else strcpy(pathMDFS,archivo_mdfs);
	int contador=0,indice_path;

	for (indice_path=0;indice_path<strlen(pathMDFS);indice_path++)	if (pathMDFS[indice_path]=='/') contador++;
	if (contador>1){
		directoriosPorSeparado=string_split(pathMDFS,"/");
		while(directoriosPorSeparado[posicionDirectorio+1]!=NULL){
			string_append(&directorioDestino,"/");
			string_append(&directorioDestino,directoriosPorSeparado[posicionDirectorio]);
			posicionDirectorio++;
		}
	}else if (contador == 1){
		directorioDestino=strdup("/");
	}else if (contador==0){
		printf ("Directorio destino mal ingresado\n");
		log_info(logger,"Error al copiar el archivo %s al MDFS",path);
		return -1;
	}

	//Buscar Directorio. Si existe se muestra mensaje de error
    uint32_t idPadre = BuscarPadre(directorioDestino);
    if(idPadre == -1){
      	printf("El directorio no existe. Se debe crear el directorio desde el menú. \n");
      	log_info(logger,"Error al copiar el archivo %s al MDFS",path);
       	return -1;
    }
    //Buscar Archivo. Si no existe se muestra mensaje de error y se debe volver al menú para crearlo
    uint32_t posArchivo = BuscarArchivoPorNombre (pathMDFS,idPadre);
    if(!(posArchivo == -1)){
     printf("El archivo ya existe. Se debe especificar un archivo nuevo. \n");
     log_info(logger,"Error al copiar el archivo %s al MDFS",path);
     return -1;
    }

    //Se debe crear un nuevo archivo con el nombre ingresado, cuyo padre sea "idPadre"
    int n_copia=0;
    int bandera;
    memset(combo.buf_20mb,'\0',BLOCK_SIZE);
    archivo_temporal=malloc(sizeof(t_archivo));
    archivo_temporal->bloques=list_create();
    printf ("Copiando archivo al MDFS, esta operacion puede tardar varios minutos\n");
    while (fread(&combo.buf_20mb,sizeof(char),sizeof(combo.buf_20mb),archivoLocal) == BLOCK_SIZE){
    		cantBytes+=BLOCK_SIZE;
    		n_copia++;
    		t_bloque *bloque_temporal=malloc(sizeof(t_bloque));
    		bloque_temporal->copias=list_create();
    		if (combo.buf_20mb[BLOCK_SIZE-1]=='\n'){
    			list_sort(nodos_temporales, (void*)nodos_mas_libres);
    			//Copiar el contenido del Buffer en los nodos mas vacios por triplicado
    			bandera=0;
    			for (indice=0;indice<list_size(nodos_temporales);indice++){
    				if (bandera==3) break;
    				nodo_temporal=list_get(nodos_temporales,indice);
    				if (nodo_temporal->estado == 1 && nodo_temporal->bloques_libres > 0){  //controlo que el nodo tenga espacio y este habilitado
    					bandera++;
    					if (send(nodo_temporal->socket, handshake, sizeof(handshake), MSG_WAITALL) == -1) {
    						perror("send handshake en funcion subir archivo");
							log_error(logger, "FALLO el envio del aviso de obtener bloque ");
							exit(-1);
						}
						corte=0;
						for(combo.n_bloque=0;combo.n_bloque<nodo_temporal->bloques_totales;combo.n_bloque++){
							if (!bitarray_test_bit(nodo_temporal->bloques_del_nodo,combo.n_bloque)){
								corte=1;
								break;
							}
						}
						if (corte==0){
							printf ("El nodo %s no tiene bloques libles, se cancela la subida del archivo\n",nodo_temporal->nodo_id);
							log_info(logger,"Error al copiar el archivo %s al MDFS",path);
							return -1;
						}
						//printf ("voy a mandar al nodo %s la copia %d del bloque %d y la guardara en el bloque %d\n",nodo_temporal->nodo_id,indice+1,n_copia,combo.n_bloque);
						if (send(nodo_temporal->socket, &combo, sizeof(combo), 0) == -1) {
							perror("send buffer en subir archivo");
							log_error(logger, "FALLO el envio del aviso de obtener bloque ");
							exit(-1);
						}
						bitarray_set_bit(nodo_temporal->bloques_del_nodo,combo.n_bloque);
						nodo_temporal->bloques_libres--;

						//Agrego la copia del bloque a la lista de copias de este bloque particular
						t_copias *copia_temporal=malloc(sizeof(t_copias));
						copia_temporal->bloqueNodo=combo.n_bloque;
						copia_temporal->nodo=strdup(nodo_temporal->nodo_id);
						list_add(bloque_temporal->copias,copia_temporal);
    				}
    			}
    			if (bandera!=3){
    				printf ("No hay suficientes nodos disponibles con espacio libre\n");
    				log_info(logger,"Error al copiar el archivo %s al MDFS",path);
    				return -1;
    			}
    			memset(combo.buf_20mb,'\0',BLOCK_SIZE);
    		}else{ //Caso en que el bloque no termina en "\n"
    			int p,aux;
    			for (p=BLOCK_SIZE-1,aux=0;p>=0;p--,aux++){
    				if (combo.buf_20mb[p]=='\n'){
    					pos=p;
    					break;
    				}
    			}
    			for(j=pos+1;j<BLOCK_SIZE;j++) combo.buf_20mb[j]='\0';
    			list_sort(nodos_temporales,(void*)nodos_mas_libres);
    			//Copiar el contenido del Buffer en los nodos mas vacios por triplicado
    			bandera=0;
    			for (indice=0;indice<list_size(nodos_temporales);indice++){
    				if (bandera==3) break;
    				nodo_temporal=list_get(nodos_temporales,indice);
    				if (nodo_temporal->estado == 1 && nodo_temporal->bloques_libres > 0){
    					bandera++;
    					if ((total_enviado=send(nodo_temporal->socket, handshake, sizeof(handshake), MSG_WAITALL)) == -1) {
    						perror("send error del envio de handshake en subir archivo");
    						log_error(logger, "FALLO el envio del aviso de obtener bloque ");
    						exit(-1);
    					}
    					//printf ("Lo que mande del handshake: %d\n",total_enviado);
    					corte=0;
    					for(combo.n_bloque=0;combo.n_bloque<nodo_temporal->bloques_totales;combo.n_bloque++){
    						if (!bitarray_test_bit(nodo_temporal->bloques_del_nodo,combo.n_bloque)){
    							corte=1;
    							break;
    						}
    					}
    					if (corte==0){
    						printf ("El nodo %s no tiene bloques libles, se cancela la subida del archivo\n",nodo_temporal->nodo_id);
    						log_info(logger,"Error al copiar el archivo %s al MDFS",path);
    						return -1;
    					}
    					//printf ("voy a mandar al nodo %s la copia %d del bloque %d y la guardara en el bloque %d\n",nodo_temporal->nodo_id,indice+1,n_copia,combo.n_bloque);
    					if ((total_enviado=send(nodo_temporal->socket, &combo, sizeof(combo), 0)) == -1) {
    						perror("send buffer en subir archivo");
    						log_error(logger, "FALLO el envio del aviso de obtener bloque ");
    						exit(-1);
    					}
    					//printf ("Quiero enviar %d y envie %d\n",sizeof(combo),total_enviado);
    					nodo_temporal->bloques_libres--;
    					bitarray_set_bit(nodo_temporal->bloques_del_nodo,combo.n_bloque);


    					//Agrego la copia del bloque a la lista de copias de este bloque particular
    					t_copias *copia_temporal=malloc(sizeof(t_copias));
    					copia_temporal->bloqueNodo=combo.n_bloque;
    					copia_temporal->nodo=strdup(nodo_temporal->nodo_id);
    					list_add(bloque_temporal->copias,copia_temporal);
    				}
    			}
    			if (bandera!=3){
    				printf ("No hay suficientes nodos disponibles con espacio libre\n");
    				log_info(logger,"Error al copiar el archivo %s al MDFS",path);
    				return -1;
    			}
    			pos = 0;
    			cantBytes-=aux;
    			fseek(archivoLocal,cantBytes,SEEK_SET);
    			memset(combo.buf_20mb,'\0',BLOCK_SIZE);
    		}
    		list_add(archivo_temporal->bloques,bloque_temporal);
    	}
    	//FIN DEL WHILE
    	if (feof(archivoLocal))
    	{
    		//aca va el fin
    		//si leyo menos lo mando de una porque seguro temina en \n y esta relleno de 0
    		t_bloque *bloque_temporal=malloc(sizeof(t_bloque));
    		bloque_temporal->copias=list_create();
    		n_copia++;
    		list_sort(nodos_temporales, (void*)nodos_mas_libres);
    		//Copiar el contenido del Buffer en los nodos mas vacios por triplicado
    		bandera=0;
    		for (indice=0;indice<list_size(nodos_temporales);indice++){
    			if (bandera==3) break;
    			nodo_temporal=list_get(nodos_temporales,indice);
    			if (nodo_temporal->estado == 1 && nodo_temporal->bloques_libres > 0){
    				bandera++;
    				if ((total_enviado=send(nodo_temporal->socket, handshake, sizeof(handshake), MSG_WAITALL)) == -1) {
    					perror("send handshake en subir archivo");
    					log_error(logger, "FALLO el envio del aviso de obtener bloque ");
    					exit(-1);
    				}
    				//printf ("Lo que mande del handshake: %d\n",total_enviado);
    				corte=0;
    				for(combo.n_bloque=0;combo.n_bloque<nodo_temporal->bloques_totales;combo.n_bloque++){
    					if (!bitarray_test_bit(nodo_temporal->bloques_del_nodo,combo.n_bloque)){
    						corte=1;
    						break;
    					}
    				}
    				if (corte==0){
    					printf ("El nodo %s no tiene bloques libles, se cancela la subida del archivo\n",nodo_temporal->nodo_id);
    					log_info(logger,"Error al copiar el archivo %s al MDFS",path);
    					return -1;
    				}
    				//printf ("voy a mandar al nodo %s la copia %d del bloque %d y la guardara en el bloque %d\n",nodo_temporal->nodo_id,indice+1,n_copia,combo.n_bloque);
    				if ((total_enviado=send(nodo_temporal->socket, &combo, sizeof(combo), 0)) == -1) {
    					perror("send buffer en subir archivo");
    					log_error(logger, "FALLO el envio del aviso de obtener bloque ");
    					exit(-1);
    				}
    				//printf ("Esto es lo que quedo, Quiero enviar %d y envie %d\n",sizeof(combo),total_enviado);
    				nodo_temporal->bloques_libres--;
    				bitarray_set_bit(nodo_temporal->bloques_del_nodo,combo.n_bloque);

    				//Agrego la copia del bloque a la lista de copias de este bloque particular
					t_copias *copia_temporal=malloc(sizeof(t_copias));
					copia_temporal->bloqueNodo=combo.n_bloque;
					copia_temporal->nodo=strdup(nodo_temporal->nodo_id);
					list_add(bloque_temporal->copias,copia_temporal);
    			}
    		}
    		if (bandera!=3){
    			printf ("No hay suficientes nodos disponibles con espacio libre\n");
    			log_info(logger,"Error al copiar el archivo %s al MDFS",path);
    			return -1;
    		}
    		list_add(archivo_temporal->bloques,bloque_temporal);
    	}
    	strcpy(ruta,pathMDFS);
    	char *nombre_del_archivo;
    	int aux1,aux2=0;
    	char *saveptr;
    	for (aux1=0;aux1<strlen(ruta);aux1++) if (ruta[aux1]=='/') aux2++;
    	nombre_del_archivo = strtok_r(ruta,"/",&saveptr);
    	for (aux1=0;aux1<aux2-1;aux1++) nombre_del_archivo = strtok_r(NULL,"/",&saveptr);
    	memset(archivo_temporal->nombre,'\0',200);
    	strcpy(archivo_temporal->nombre,nombre_del_archivo);
    	archivo_temporal->padre=idPadre;
    	fclose(archivoLocal);

    	if(marta_presente == 1){
    		memset(identificacion,'\0',BUF_SIZE);
    		strcpy(identificacion, "nuevo_arch");
    		if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		if ((send(marta_sock, archivo_temporal->nombre,sizeof(archivo_temporal->nombre), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		if ((send(marta_sock, &archivo_temporal->padre,sizeof(uint32_t), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		int cantidad_bloques=list_size(archivo_temporal->bloques);
    		if ((send(marta_sock, &cantidad_bloques,sizeof(int), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}

    		for (cantidad_bloques=0;cantidad_bloques<list_size(archivo_temporal->bloques);cantidad_bloques++){
    			t_bloque *unBloque;
    			unBloque=list_get(archivo_temporal->bloques,cantidad_bloques);
    			int cantidad_copias=list_size(unBloque->copias);
    			if ((send(marta_sock, &cantidad_copias,sizeof(int), MSG_WAITALL)) == -1) {
    				perror("send");
    				log_error(logger, "FALLO el envio del ok a Marta");
    				exit(-1);
    			}
    			for (cantidad_copias=0;cantidad_copias<list_size(unBloque->copias);cantidad_copias++){
    				t_copias *unaCopia;
    				unaCopia=list_get(unBloque->copias,cantidad_copias);
    				char nodo_id_para_enviar[6];
    				memset(nodo_id_para_enviar,'\0',6);
    				strcpy(nodo_id_para_enviar,unaCopia->nodo);
    				if ((send(marta_sock, nodo_id_para_enviar,sizeof(nodo_id_para_enviar), MSG_WAITALL)) == -1) {
    					perror("send");
    					log_error(logger, "FALLO el envio del ok a Marta");
    					exit(-1);
    				}
    				if ((send(marta_sock, &unaCopia->bloqueNodo,sizeof(int), MSG_WAITALL)) == -1) {
    					perror("send");
    					log_error(logger, "FALLO el envio del ok a Marta");
    					exit(-1);
    				}
    			}
    		}
    	}
    	persistir_archivo(archivo_temporal);
    	//printf ("Pasa persistencia\n");
    	//Si llego hasta aca salio tod0 bien, actualizo la lista real de nodos
    	eliminar_listas(NULL,NULL,nodos);
    	nodos=list_create();
    	if (copiar_lista_de_nodos(nodos,nodos_temporales)){
    		printf ("No se pudo crear la copia de la lista de nodos\n");
    		return -1;
    	}
    	//printf ("Copia lista de nodos\n");
    	eliminar_listas(NULL,NULL,nodos_temporales);
    	//printf ("Pasa eliminar lista nodos\n");


    	//Si llego aca es porque tod0 salio bien y actualizo la lista de archivos
    	list_add(archivos_temporales,archivo_temporal);
    	eliminar_listas(archivos,NULL,NULL);
    	archivos=list_create();
    	if (copiar_lista_de_archivos(archivos,archivos_temporales)){
    		printf ("No se pudo crear la copia de la lista de archivos\n");
    		return -1;
    	}
    	//printf ("Pasa copia lista archivos\n");
    	eliminar_listas(archivos_temporales,NULL,NULL);
    	log_info(logger,"El archivo %s se copio correctamente al MDFS",path);
    	loguear_estado_de_los_nodos(nodos);
    	loguear_espacio_del_sistema(nodos);
    	printf ("El archivo %s se copio correctamente al MDFS\n",path);
    	return 0;
}
int CopiarArchivoDelMDFS(int flag, char*unArchivo) {
	if (flag!=99) 	printf("Eligió Copiar un archivo del MDFS al filesystem local\n");
	int i;
	t_archivo *archivo;
	if (list_size(archivos)!=0){
		if (flag!=99){
			printf ("Se listan los archivos existentes en MDFS:\n");
			for (i = 0; i < list_size(archivos); i++) {
				archivo = list_get(archivos, i);
				printf("\n");
				printf(".....Archivo: %s Padre: %d Bloques: %d Path: %s\n",archivo->nombre,archivo->padre,list_size(archivo->bloques),obtenerPath(archivo->nombre, archivo->padre));
				//printf(".....Archivo: %s Padre: %d Bloques: %d\n",archivo->nombre,archivo->padre,list_size(archivo->bloques));
			}
		}
		char pathArchivo[200];
		memset(pathArchivo,'\0',200);
		FILE* copiaLocal;
		t_bloque *bloque;
		t_copias *copia;
		int j,k;
		int bloqueDisponible;
		int socket_nodo;
		char* bloqueParaVer;
		char ruta[200];
		char *nombre_del_archivo=string_new();
		int aux1,aux2=0;
		char *saveptr;
		char ruta_local[200];
		char **directoriosPorSeparado;
		int posicionDirectorio=0;
		char *directorio=string_new();
		if (flag!=99){
			printf("\nEligió Copiar un archivo del MDFS al filesystem local\n");
			printf ("Ingrese el archivo a copiar con su path completo, ej. /directorio/archivo.ext\n");
			scanf("%s",pathArchivo);
			log_info(logger,"Selecciono copiar el archivo %s Del MDFS",pathArchivo);
		}else strcpy(pathArchivo,unArchivo);
		strcpy(ruta,pathArchivo);
		for (aux1=0;aux1<strlen(ruta);aux1++) if (ruta[aux1]=='/') aux2++;
		nombre_del_archivo = strtok_r(ruta,"/",&saveptr);
		for (aux1=0;aux1<aux2-1;aux1++) nombre_del_archivo = strtok_r(NULL,"/",&saveptr);
		strcpy(ruta_local,"/tmp/");
		strcat(ruta_local,nombre_del_archivo);


		int contador=0,indice_path;
		for (indice_path=0;indice_path<strlen(pathArchivo);indice_path++)	if (pathArchivo[indice_path]=='/') contador++;
		if (contador>1){
			directoriosPorSeparado=string_split(pathArchivo,"/");
			while(directoriosPorSeparado[posicionDirectorio+1]!=NULL){
				string_append(&directorio,"/");
				string_append(&directorio,directoriosPorSeparado[posicionDirectorio]);
				posicionDirectorio++;
			}
		}else if (contador == 1){
			directorio=strdup("/");
		}else if (contador==0){
			printf ("Directorio destino mal ingresado\n");
			log_info(logger,"Error al copiar el archivo %s Del MDFS",pathArchivo);
			return -1;
		}

		int idPadre = BuscarPadre(directorio);
		if (idPadre==-1){
			printf("El directorio no existe\n");
			log_info(logger,"Error al copiar el archivo %s Del MDFS",pathArchivo);
			return -1;
		}
		int posArchivo = BuscarArchivoPorNombre(pathArchivo, idPadre);
		if (posArchivo==-1){
			printf ("El archivo no existe\n");
			log_info(logger,"Error al copiar el archivo %s Del MDFS",pathArchivo);
			return -1;
		}
		archivo = list_get(archivos, posArchivo);
		loguear_lista_de_bloques_de_archivo(archivo->nombre,archivo->padre);
		copiaLocal = fopen(ruta_local, "w");
		for (j=0;j<list_size(archivo->bloques);j++){
			bloque=list_get(archivo->bloques,j);
			bloqueDisponible=0;
			for (k=0;k<list_size(bloque->copias);k++){
				copia=list_get(bloque->copias,k);
				if(obtenerEstadoDelNodo(copia->nodo)==1 && obtenerEstadoDelBloque(copia->nodo,copia->bloqueNodo)==1){
					bloqueDisponible=1;
					socket_nodo =obtener_socket_de_nodo_con_id(copia->nodo);
					if (socket_nodo == -1){
						log_error(logger, "El nodo ingresado no es valido o no esta disponible\n");
						printf("El nodo %s no esta disponible\n",copia->nodo);
						return -1;
					}
					enviarNumeroDeBloqueANodo(socket_nodo, copia->bloqueNodo);
					bloqueParaVer = recibirBloque(socket_nodo);
					fprintf(copiaLocal,"%s",bloqueParaVer);
					break;
				}
			}
			if (bloqueDisponible==0){
				printf ("El archivo no se puede recuperar, el bloque %d no esta disponible\n",j);
				log_info(logger,"Error al copiar el archivo %s Del MDFS",pathArchivo);
				return -1;
			}
		}
		fclose(copiaLocal);
		if (flag!=99) printf ("El archivo se copio exitosamente\n");
		log_info(logger,"el archivo %s se copio correctamente Del MDFS",pathArchivo);
		return 0;
	}else{
		if (flag!=99){
			printf ("No hay archivos cargados en MDFS\n");
			return -1;
		}else return -1;
	}
}
int obtenerEstadoDelBloque(char *nodo,int bloqueNodo){
	t_nodo *unNodo;
		int i;
		for (i=0;i<list_size(nodos);i++){
			unNodo=list_get(nodos,i);
			if (bloqueNodo <= unNodo->bloques_totales)
				if ((strcmp(unNodo->nodo_id,nodo)==0) && bitarray_test_bit(unNodo->bloques_del_nodo,bloqueNodo)==1) return 1;
		}
		return -1;
}

int obtenerEstadoDelNodo(char* nodo){
	t_nodo *unNodo;
	int i;
	for (i=0;i<list_size(nodos);i++){
		unNodo=list_get(nodos,i);
		if ((strcmp(unNodo->nodo_id,nodo)==0) && (unNodo->estado_red==1) && (unNodo->estado==1)) return 1;
	}
	return -1;
}

void MD5DeArchivo() {
	printf("Eligió Solicitar el MD5 de un archivo en MDFS\n");
	if (list_size(archivos)!=0){
		int i;
		t_archivo *archivo;
		printf ("Se listan los archivos existentes en MDFS:\n");
		for (i = 0; i < list_size(archivos); i++) {
			archivo = list_get(archivos, i);
			printf("\n");
			printf(".....Archivo: %s Padre: %d Bloques: %d Path: %s\n",archivo->nombre,archivo->padre,list_size(archivo->bloques),obtenerPath(archivo->nombre, archivo->padre));
			//printf(".....Archivo: %s Padre: %d Bloques: %d\n",archivo->nombre,archivo->padre,list_size(archivo->bloques));
		}
		int fd[2];
		int childpid;
		pipe(fd);
		char result[50];
		char path[200];
		memset(result,'\0',50);
		memset(path,'\0',200);
		char ruta[200];
		memset(ruta,'\0',200);
		char *nombre_del_archivo=string_new();
		char ruta_local[100];
		memset(ruta_local,'\0',100);
		int aux1,aux2=0;
		char *saveptr;
		printf ("\nIngrese el path del archivo en MDFS:\n");
		scanf ("%s",path);
		log_info(logger,"Solicito calcular el MD5 del archivo %s Del MDFS",path);

		if(CopiarArchivoDelMDFS(99,path)==-1){
			printf ("El archivo seleccionado no esta disponible\n");
			log_info(logger,"Error al calcular el MD5 del archivo %s Del MDFS",path);
			return;
		}

		strcpy(ruta,path);
		for (aux1=0;aux1<strlen(ruta);aux1++) if (ruta[aux1]=='/') aux2++;
		nombre_del_archivo = strtok_r(ruta,"/",&saveptr);
		for (aux1=0;aux1<aux2-1;aux1++) nombre_del_archivo = strtok_r(NULL,"/",&saveptr);
		strcpy(ruta_local,"/tmp/");
		strcat(ruta_local,nombre_del_archivo);
		if ( (childpid = fork() ) == -1){
			fprintf(stderr, "FORK failed");
		} else if( childpid == 0) {
			close(1);
			dup2(fd[1], 1);
			close(fd[0]);
			execlp("/usr/bin/md5sum","md5sum",ruta_local,NULL);
		}
		wait(NULL);
		read(fd[0], result, sizeof(result));
		printf("%s\n",result);
		log_info(logger,"El MD5 del archivo %s Del MDFS se calculo correctamente %s",path,result);
	}else printf ("No hay archivos cargados en MDFS\n");

}


void BorrarBloque() {
	int i, cantNodos, bloque, nodoEncontrado;
	nodoEncontrado = 0;
	t_nodo* nodoBuscado;
	int k;
	t_archivo *unArchivo;
	t_bloque *unBloque;
	t_copias *unaCopia;
	int bloque_encontrado=0;
	cantNodos = list_size(nodos);
	char nodoId[6];
	memset(nodoId,'\0',6);
	printf("Ingrese el ID del nodo del que desea borrar un bloque:\n");
	scanf("%s", nodoId);
	printf("Ingrese el número de bloque que desea borrar:\n");
	scanf("%d", &bloque);
	i = 0;
	log_info(logger,"Selecciono borrar el bloque %d del nodo %s",bloque,nodoId);
	while (i < cantNodos && nodoEncontrado == 0) {
		nodoBuscado = list_get(nodos, i);
		if (strcmp(nodoBuscado->nodo_id, nodoId)==0 && bitarray_test_bit(nodoBuscado->bloques_del_nodo,bloque)==1) {
			nodoEncontrado = 1;
		}
		i++;
	}
	if (nodoEncontrado == 1){
		nodoBuscado->bloques_libres++;
		bitarray_clean_bit(nodoBuscado->bloques_del_nodo, bloque);

		//Elimino la copia del bloque correspondiente del archivo al que pertenece
		for (i=0;i<list_size(archivos);i++){
			unArchivo=list_get(archivos,i);
			for(j=0;j<list_size(unArchivo->bloques);j++){
				unBloque=list_get(unArchivo->bloques,j);
				for(k=0;k<list_size(unBloque->copias);k++){
					unaCopia=list_get(unBloque->copias,k);
					if (unaCopia->bloqueNodo==bloque && strcmp(unaCopia->nodo,nodoId)==0){
						bloque_encontrado=1;

						break;
					}
				}
				if (bloque_encontrado==1){
					list_remove_and_destroy_element(unBloque->copias,k,(void*)eliminar_lista_de_copias);
					break;
				}
			}
			if (bloque_encontrado==1) break;
		}
		printf("Se ha borrado el bloque correctamente\n");
		log_info(logger,"El bloque %d del nodo %s fue eliminado correctamente",bloque,nodoId);
		loguear_estado_de_los_nodos(nodos);
		loguear_espacio_del_sistema(nodos);

		//actualizar persistencia
		actualizar_persistencia_eliminar_bloque(nodoId,bloque);

		//Actualizar en marta
		if(marta_presente == 1){
    		memset(identificacion,'\0',BUF_SIZE);
    		strcpy(identificacion, "elim_bloque");
    		if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		if ((send(marta_sock, unArchivo->nombre,sizeof(unArchivo->nombre), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		if ((send(marta_sock, &unArchivo->padre,sizeof(unArchivo->padre), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		if ((send(marta_sock, nodoId,sizeof(nodoId), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
    		if ((send(marta_sock, &bloque,sizeof(bloque), MSG_WAITALL)) == -1) {
    			perror("send");
    			log_error(logger, "FALLO el envio del ok a Marta");
    			exit(-1);
    		}
		}

	}
	else{
		printf("No se puede eliminar el bloque, el nodo no existe o el bloque esta vacio\n");
		log_info(logger,"No se pudo eliminar el bloque %d del nodo %s",bloque,nodoId);
		}
}


void CopiarBloque() {
	char nodo_origen[6];
	memset(nodo_origen,'\0',6);
	char nodo_destino[6];
	memset(nodo_destino,'\0',6);
	int bloque_origen, bloque_destino;
	char handshake[15]="copiar_archivo";
	int i,j,k;
	char *bloqueParaVer=string_new();
	int origen_encontrado=0, destino_encontrado=0;
	int socket_nodo;
	printf("Eligió Copiar un bloque de un archivo\n");
	printf ("Ingrese el nodo de origen:\n");
	scanf ("%s",nodo_origen);
	printf ("Ingrese bloque de origen:\n");
	scanf("%d",&bloque_origen);
	printf ("Ingrese el nodo de destino:\n");
	scanf ("%s",nodo_destino);
	printf ("Ingrese bloque de destino:\n");
	scanf ("%d",&bloque_destino);

	t_nodo *origen;
	t_nodo *destino;

	if(!obtenerEstadoDelNodo(nodo_origen)){
		printf ("El nodo seleccionado como origen no esta disponible o no existe\n");
		log_info(logger,"No se puede copiar el bloque - El nodo seleccionado como origen no esta disponible o no existe");
		return;
	}
	if(!obtenerEstadoDelNodo(nodo_destino)){
		printf ("El nodo seleccionado como destino no esta disponible o no existe\n");
		log_info(logger,"No se puede copiar el bloque - El nodo seleccionado como destino no esta disponible o no existe");
		return;
	}
	for (i=0;i<list_size(nodos);i++){
		origen=list_get(nodos,i);
		if (strcmp(origen->nodo_id,nodo_origen)==0){
			if (origen->estado==1 && origen->estado_red==1) origen_encontrado=1;
			if (!bitarray_test_bit(origen->bloques_del_nodo,bloque_origen)){
				printf ("El bloque %d del nodo %s esta vacio, no hay nada que copiar\n",bloque_origen,nodo_origen);
				log_info(logger,"No se puede copiar el bloque - El bloque de origen esta vacio");
				return;
			}
			break;
		}
	}

	for (i=0;i<list_size(nodos);i++){
		destino=list_get(nodos,i);
		if (strcmp(destino->nodo_id,nodo_destino)==0){
			if (destino->estado==1 && destino->estado_red==1) destino_encontrado=1;
			if (bitarray_test_bit(destino->bloques_del_nodo,bloque_destino)){
				printf ("El bloque %d del nodo %s esta ocupado, no se puede copiar\n",bloque_destino,nodo_destino);
				log_info(logger,"No se puede copiar el bloque - El bloque de destino no esta vacio");
				return;
			}
			break;
		}
	}
	if (origen_encontrado==0){
		printf ("El nodo origen no existe o no esta disponible\n");
		log_info(logger,"No se puede copiar el bloque - El nodo de origen no esta disponible");
		return;
	}
	if (destino_encontrado==0){
		printf ("El nodo destino no existe o no esta disponible\n");
		log_info(logger,"No se puede copiar el bloque - El nodo de destino no esta disponible");
		return;
	}
	//obtener primero el bloque del nodo original
	socket_nodo = obtener_socket_de_nodo_con_id(nodo_origen);
	if (socket_nodo == -1){
		log_error(logger, "El nodo ingresado como origen no es valido o no esta disponible\n");
		printf("El nodo ingresado como origen no es valido o no esta disponible\n");
		return;
	}
	enviarNumeroDeBloqueANodo(socket_nodo, bloque_origen);
	bloqueParaVer = recibirBloque(socket_nodo);
	memset(combo.buf_20mb,'\0',BLOCK_SIZE);
	strcpy(combo.buf_20mb,bloqueParaVer);

	//copiar el bloque obtenido en el nodo destino en el bloque especificado
	//luego actualizar la estructura de copias del bloque destino del nodo destino
	//no voy a usar copias de estructuras ya que previamente valido todos

	socket_nodo = obtener_socket_de_nodo_con_id(nodo_destino);
	if (socket_nodo == -1){
		log_error(logger, "El nodo ingresado como destino no es valido o no esta disponible\n");
		printf("El nodo ingresado como destino no es valido o no esta disponible\n");
		return;
	}
	if (send(socket_nodo, handshake, sizeof(handshake), MSG_WAITALL) == -1) {
		perror("send handshake en funcion copiar bloque");
		log_error(logger, "FALLO el envio del aviso de obtener bloque ");
		exit(-1);
	}
	printf ("voy a mandar al nodo %s una copia del bloque %d\n",nodo_destino,bloque_origen);
	combo.n_bloque=bloque_destino;
	if (send(socket_nodo, &combo, sizeof(combo), 0) == -1) {
		perror("send buffer en subir archivo");
		log_error(logger, "FALLO el envio del aviso de obtener bloque ");
		exit(-1);
	}
	bitarray_set_bit(destino->bloques_del_nodo,bloque_destino);
	destino->bloques_libres--;

	//Agrego la copia del bloque a la lista de copias de este bloque particular
	t_copias *copia_temporal=malloc(sizeof(t_copias));
	copia_temporal->bloqueNodo=bloque_destino;
	copia_temporal->nodo=strdup(nodo_destino);

	//Busco el bloque en el destino para agregar la nueva copia del bloque
	t_archivo *unArchivo;
	t_bloque *unBloque;
	t_copias *unaCopia;
	int bloque_encontrado=0;
	for (i=0;i<list_size(archivos);i++){
		unArchivo=list_get(archivos,i);
		for(j=0;j<list_size(unArchivo->bloques);j++){
			unBloque=list_get(unArchivo->bloques,j);
			for(k=0;k<list_size(unBloque->copias);k++){
				unaCopia=list_get(unBloque->copias,k);
				if (unaCopia->bloqueNodo==bloque_origen && strcmp(unaCopia->nodo,nodo_origen)==0){
					bloque_encontrado=1;
					break;
				}
			}
			if (bloque_encontrado==1){
				list_add(unBloque->copias,copia_temporal);
				actualizar_persistencia_copiar_bloque(nodo_origen,bloque_origen,nodo_destino,bloque_destino);
				break;
			}
		}
		if (bloque_encontrado==1) break;
	}
	if (bloque_encontrado==1){
		//actualizo a marta
		if(marta_presente == 1){
			memset(identificacion,'\0',BUF_SIZE);
			strcpy(identificacion, "nuevo_bloque");
			if ((send(marta_sock, identificacion,sizeof(identificacion), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, unArchivo->nombre,sizeof(unArchivo->nombre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}

			if ((send(marta_sock, &unArchivo->padre,sizeof(unArchivo->padre), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, &j,sizeof(j), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}

			if ((send(marta_sock, nodo_destino,sizeof(nodo_destino), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}

			if ((send(marta_sock, &bloque_destino,sizeof(bloque_destino), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}

		}
		printf ("Bloque copiado exitosamente\n");
		log_info(logger, "La operacion de copiar bloque termino exitosamente");
		loguear_estado_de_los_nodos(nodos);
		loguear_espacio_del_sistema(nodos);
	}
}

void AgregarNodo(){
	//printf("Eligió Agregar un nodo de datos\n");
	int i,cantNodos, nodoEncontrado;
	nodoEncontrado =0; //0 no lo encontró, 1 lo encontró
	t_nodo* nodoAEvaluar;
	char nodoID[6];
	memset(nodoID,'\0',6);
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
		if (strcmp(nodoAEvaluar->nodo_id, nodoID)==0 && nodoAEvaluar->estado_red == 1 && nodoAEvaluar->estado == 0) {
			nodoEncontrado = 1;
		}
		i++;
	}
	if (nodoEncontrado == 1){
		modificar_estado_nodo(nodoAEvaluar->nodo_id, nodoAEvaluar->socket, nodoAEvaluar->puerto, 1, 99); //cambio su estado de la lista a 1 que es activo, invoco con 99 para solo cambiar estado
		printf("Se ha agregado el nodo %s correctamente\n",nodoID);
		if(marta_presente == 1){
			char identificacion_marta[BUF_SIZE];
			memset(identificacion_marta,'\0',BUF_SIZE);
			strcpy(identificacion_marta, "nodo_agre");
			if ((send(marta_sock, identificacion_marta,sizeof(identificacion_marta), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, nodoAEvaluar->nodo_id,sizeof(nodoAEvaluar->nodo_id), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
		}
		loguear_estado_de_los_nodos(nodos);
		loguear_espacio_del_sistema(nodos);
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
	char nodoID[6];
	memset(nodoID,'\0',6);

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
		if (strcmp(nodoAEvaluar->nodo_id, nodoID)==0 && nodoAEvaluar->estado_red == 1 && nodoAEvaluar->estado == 1) {
		nodoEncontrado = 1;
	}
		i++;
	}
	if (nodoEncontrado == 1){
		modificar_estado_nodo(nodoAEvaluar->nodo_id, nodoAEvaluar->socket, nodoAEvaluar->puerto, 0, 99); //cambio su estado de la lista a 0 que es inactivo, invoco con 99 para solo cambiar estado
		printf("Se ha eliminado el nodo %s correctamente\n",nodoID);

		if(marta_presente == 1){
			char identificacion_marta[BUF_SIZE];
			memset(identificacion_marta,'\0',BUF_SIZE);
			strcpy(identificacion_marta, "nodo_elim");
			if ((send(marta_sock, identificacion_marta,sizeof(identificacion_marta), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
			if ((send(marta_sock, nodoAEvaluar->nodo_id,sizeof(nodoAEvaluar->nodo_id), MSG_WAITALL)) == -1) {
				perror("send");
				log_error(logger, "FALLO el envio del ok a Marta");
				exit(-1);
			}
		}
		loguear_estado_de_los_nodos(nodos);
		loguear_espacio_del_sistema(nodos);
	}
	else{
		printf("El nodo ingresado no se puede eliminar\n");
	}
}

int obtener_socket_de_nodo_con_id(char *id) {
	int i, cantidad_nodos;
	t_nodo *elemento;
	cantidad_nodos = list_size(nodos);
	for (i = 0; i < cantidad_nodos; i++) {
		elemento = list_get(nodos, i);
		if (strcmp(elemento->nodo_id, id) == 0 && elemento->estado == 1	&& elemento->estado_red == 1)
			return elemento->socket;
	}
	return -1;
}

int VerBloque() {
	FILE* archivoParaVerPath;
	char * bloqueParaVer;
	int nroBloque, i, cantNodos;
	int socket_nodo;
	t_nodo *nodoAEvaluar;
	bloqueParaVer = malloc(BLOCK_SIZE);
	printf("Ingrese id de Nodo: ");
	scanf("%s", nodo_id);
	printf("Numero de Bloque que desea ver: ");
	scanf("%d", &nroBloque);
	log_error(logger, "Selecciono la opcion ver el bloque %d del nodo %s",nroBloque,nodo_id);
	cantNodos= list_size(nodos);
	for (i=0;i<cantNodos;i++){
		nodoAEvaluar = list_get(nodos,i);
		if(strcmp(nodoAEvaluar->nodo_id,nodo_id)==0){
			if (nodoAEvaluar->estado_red == 1 && nodoAEvaluar->estado == 1){
				if(bitarray_test_bit(nodoAEvaluar->bloques_del_nodo, nroBloque)== 0){
					log_error(logger,"Esta queriendo ver un bloque vacio");
					printf("Esta queriendo ver un bloque vacio\n");
					return -1;
				}

				socket_nodo =obtener_socket_de_nodo_con_id(nodo_id);
				if (socket_nodo == -1){
					log_error(logger, "El nodo ingresado no es valido o no esta disponible\n");
					printf("El nodo ingresado no es valido o no esta disponible\n");
					return -1;
				}
				enviarNumeroDeBloqueANodo(socket_nodo, nroBloque);
				bloqueParaVer = recibirBloque(socket_nodo);
				archivoParaVerPath = fopen("./archBloqueParaVer.txt", "w");
				fprintf(archivoParaVerPath, "%s", bloqueParaVer);
				printf("El bloque se copio en el archivo: ./archBloqueParaVer.txt\n");
				log_error(logger, "La operacion ver el bloque %d del nodo %s termino correctamente",nroBloque,nodo_id);
				fclose(archivoParaVerPath);
				free(bloqueParaVer);
				break;
			}else{
				log_error(logger, "El nodo donde esta el bloque que quiere ver no esta disponible\n");
				printf("El nodo donde esta el bloque que quiere ver no esta disponible\n");
				return -1;
			}
		}
	}
	return 0;
}

void enviarNumeroDeBloqueANodo( int socket_nodo, int bloque) {
	strcpy(identificacion, "obtener bloque");
	if (send(socket_nodo, identificacion, sizeof(identificacion), MSG_WAITALL) == -1) {
		perror("send");
		log_error(logger, "FALLO el envio del aviso de obtener bloque ");
		exit(-1);
	}
	if (send(socket_nodo, &bloque, sizeof(int), MSG_WAITALL) == -1) {
		perror("send");
		log_error(logger, "FALLO el envio del numero de bloque");
		exit(-1);

	}
}
char *recibirBloque( socket_nodo) {
	char* bloqueAObtener;
		bloqueAObtener = malloc(BLOCK_SIZE);
		if (recv(socket_nodo, bloqueAObtener, BLOCK_SIZE, MSG_WAITALL) == -1) {
			perror("recv");
			log_error(logger, "FALLO el Recv del bloque por parte del nodo");
			exit(-1);
		}
	return bloqueAObtener;
}
