#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <commons/log.h>
#include <commons/config.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <commons/collections/list.h>
#include "FS_MDFS.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#define BUF_SIZE 50
#define BLOCK_SIZE 20971520
#define MENSAJE_SIZE 4096

//Prototipos de Funciones
int Menu();
void DibujarMenu();
void *connection_handler_escucha(); // Esta funcion escucha continuamente si recibo nuevos mensajes
static t_nodo *agregar_nodo_a_lista(int socket,char *nodo_id,int est,char *ip, int port, int bloques_lib, int bloques_tot);
char *asignar_nombre_a_nodo();
void modificar_estado_nodo (int socket,char *ip,int port,int estado);
void formatear_nodos(void);
void FormatearFilesystem ();		//TODAVIA NO DESARROLLADA
void EliminarArchivo();				//TODAVIA NO DESARROLLADA
void RenombrarArchivo ();			//TODAVIA NO DESARROLLADA
void MoverArchivo();				//TODAVIA NO DESARROLLADA
void CrearDirectorio();				//TODAVIA NO DESARROLLADA
void EliminarDirectorio();			//TODAVIA NO DESARROLLADA
void RenombrarDirectorio();			//TODAVIA NO DESARROLLADA
void MoverDirectorio();				//TODAVIA NO DESARROLLADA
void CopiarArchivoAMDFS();			//TODAVIA NO DESARROLLADA
void CopiarArchivoDelMDFS();		//TODAVIA NO DESARROLLADA
void MD5DeArchivo();				//TODAVIA NO DESARROLLADA
void VerBloques();					//TODAVIA NO DESARROLLADA
void BorrarBloques();				//TODAVIA NO DESARROLLADA
void CopiarBloques();				//TODAVIA NO DESARROLLADA
void AgregarNodo();					//TODAVIA NO DESARROLLADA
void EliminarNodo();  				//TODAVIA NO DESARROLLADA
uint32_t BuscarArchivoPorNombre ();    //DESARROLLADA
uint32_t BuscarPadre ();            //TODAVIA NO DESARROLLADA

fd_set master; // conjunto maestro de descriptores de fichero
fd_set read_fds; // conjunto temporal de descriptores de fichero para select()
t_log* logger;
t_list *nodos; //lista de nodos conectados al fs
t_list* archivos; //lista de archivos del FS
t_config * configurador;
int fdmax; // número máximo de descriptores de fichero
int listener; // descriptor de socket a la escucha
int marta_sock; //Socket exclusivo para marta
struct sockaddr_in filesystem; // dirección del servidor
struct sockaddr_in remote_client; // dirección del cliente
char identificacion[BUF_SIZE]; // buffer para datos del cliente
char mensaje[MENSAJE_SIZE];
int cantidad_nodos=0;
int cantidad_nodos_historico=0;
int read_size;

int main(int argc , char *argv[]){

	pthread_t escucha; //Hilo que va a manejar los mensajes recibidos
	int newfd;
	int addrlen;
	int yes=1; // para setsockopt() SO_REUSEADDR, más abajo
	configurador= config_create("resources/fsConfig.conf"); //se asigna el archivo de configuración especificado en la ruta
	FD_ZERO(&master); // borra los conjuntos maestro y temporal
	FD_ZERO(&read_fds);

	if ((listener = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		log_info(logger,"FALLO la creacion del socket");
		exit(-1);
	}
	// obviar el mensaje "address already in use" (la dirección ya se está usando)
	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes,sizeof(int)) == -1) {
		perror("setsockopt");
		log_info(logger,"FALLO la ejecucion del setsockopt");
		exit(-1);
	}
	// enlazar
	filesystem.sin_family = AF_INET;
	filesystem.sin_addr.s_addr = INADDR_ANY;
	filesystem.sin_port = htons(config_get_int_value(configurador,"PUERTO_LISTEN"));
	memset(&(filesystem.sin_zero), '\0', 8);
	if (bind(listener, (struct sockaddr *)&filesystem, sizeof(filesystem)) == -1) {
		perror("bind");
		log_info(logger,"FALLO el Bind");
		exit(-1);
	}
	// escuchar
	if (listen(listener, 10) == -1) {
		perror("listen");
		log_info(logger,"FALLO el Listen");
		exit(1);
	}
	// añadir listener al conjunto maestro
	FD_SET(listener, &master);

	// seguir la pista del descriptor de fichero mayor
	fdmax = listener; // por ahora es éste el ultimo socket
	addrlen = sizeof(struct sockaddr_in);
	nodos=list_create(); //Crea la lista de que va a manejar la lista de nodos
	printf ("Esperando las conexiones de los nodos iniciales\n");
	while (cantidad_nodos != config_get_int_value(configurador,"CANTIDAD_NODOS")){
		if ((newfd = accept(listener, (struct sockaddr*)&remote_client, (socklen_t*)&addrlen)) == -1) {
			perror ("accept");
			log_info(logger,"FALLO el ACCEPT");
		   	exit (-1);
		}
		if ((read_size = recv(newfd, identificacion , 50 , 0))==-1) {
			perror ("recv");
			log_info(logger,"FALLO el RECV");
			exit (-1);
		}
		if (read_size > 0 && strncmp(identificacion,"nuevo",5)==0){
			cantidad_nodos++;
			cantidad_nodos_historico=cantidad_nodos;
			FD_SET(newfd, &master);
			fdmax = newfd;
			list_add (nodos, agregar_nodo_a_lista(newfd,asignar_nombre_a_nodo(),0,inet_ntoa(remote_client.sin_addr),remote_client.sin_port,50,50));
			printf ("Se conecto el nodo %s\n",inet_ntoa(remote_client.sin_addr));
		}
		else{
			printf ("Marta se quiso conectar antes de tiempo\n");
			close(newfd);
		}
	}
	printf ("Nodos conectados %d\n",list_size(nodos));
	printf ("Informacion del nodo 2\n");
	printf ("Socket %d, ip %s, Estado %d, Nodos Libres %d\n",((t_nodo*)list_get(nodos,1))->socket,((t_nodo*)list_get(nodos,1))->ip,((t_nodo*)list_get(nodos,1))->estado,((t_nodo*)list_get(nodos,1))->bloques_libres);
	sleep(5);
	//Cuando sale de este ciclo el proceso FileSystem ya se encuentra en condiciones de iniciar sus tareas

	//Este hilo va a manejar las conexiones de forma paralela a la ejecucion del proceso
	if( pthread_create( &escucha , NULL ,  connection_handler_escucha , NULL) < 0){
	       perror("could not create thread");
	       return 1;
	}
	archivos=list_create(); //Crea la lista de archivos
	Menu();
	log_destroy(logger);
	return 0;
}

//Consola Menu
void DibujarMenu(void){
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

int Menu(void){
	char opchar[20];
	int opcion=0;
	while (opcion !=17){
	    sleep(1);
	    DibujarMenu();
	    printf("Ingrese opción: ");
	    scanf ("%s", opchar);
	    opcion = atoi (opchar);
	    switch (opcion){
	      case 1: FormatearFilesystem (); break;
	      case 2: EliminarArchivo(); break;
	      case 3: RenombrarArchivo (); break;
	      case 4: MoverArchivo(); break;
	      case 5: CrearDirectorio(); break;
	      case 6:  EliminarDirectorio(); break;
	      case 7: RenombrarDirectorio(); break;
	      case 8:  MoverDirectorio(); break;
	      case 9:  CopiarArchivoAMDFS(); break;
	      case 10: CopiarArchivoDelMDFS(); break;
	      case 11: MD5DeArchivo(); break;
	      case 12: VerBloques(); break;
	      case 13: BorrarBloques(); break;
	      case 14: CopiarBloques(); break;
	      case 15: AgregarNodo(); break;
	      case 16: EliminarNodo(); break;
	      case 17: printf("Eligió Salir\n"); break;
	      default: printf("Opción incorrecta. Por favor ingrese una opción del 1 al 17\n");break;
		}
	}
	return 0;
}
static t_nodo *agregar_nodo_a_lista(int socket,char *nodo_id,int est,char *ip, int port, int bloques_lib, int bloques_tot){
	t_nodo *nodo_temporal = malloc (sizeof(t_nodo));
	nodo_temporal->socket = socket;
	nodo_temporal->nodo_id = strdup(ip);
	nodo_temporal->estado = est;
	nodo_temporal->ip = strdup (ip);
	nodo_temporal->puerto = port;
	nodo_temporal->bloques_libres = bloques_lib;
	nodo_temporal->bloques_totales = bloques_tot;
	return nodo_temporal;
}

void *connection_handler_escucha(void){
	int i,nbytes,newfd,addrlen;
	while(1) {
		read_fds = master;
		if (select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1) {
			perror("select");
			log_info(logger,"FALLO el Select");
			exit(-1);
		}
		// explorar conexiones existentes en busca de datos que leer
		for(i = 0; i <= fdmax; i++) {
			if (FD_ISSET(i, &read_fds)) { // ¡¡tenemos datos!!
				if (i == listener) {
					// gestionar nuevas conexiones, primero hay que aceptarlas
					addrlen = sizeof(struct sockaddr_in);
					if ((newfd = accept(listener, (struct sockaddr*)&remote_client,(socklen_t*)&addrlen)) == -1) {
						perror("accept");
						log_info(logger,"FALLO el ACCEPT");
						exit(-1);
					}
					else //llego una nueva conexion, se acepto y ahora tengo que tratarla
					{
						FD_SET(newfd, &master); // añadir al conjunto maestro
						if (newfd > fdmax) { // actualizar el máximo
							fdmax = newfd;
						}
						if ((nbytes = recv(newfd, mensaje, sizeof(mensaje), 0)) <= 0) { //si entra aca es porque hubo un error, no considero desconexion porque es nuevo
							{
								perror("recv");
								log_info(logger,"FALLO el Recv");
								exit(-1);
							}
						} else {
							// el nuevo conectado me manda algo, se identifica como nodo nuevo o nodo reconectado
							// luego de que se identifique lo agregare a la lista de nodos si es nodo nuevo
							// si es nodo reconectado hay que cambiarle el estado
							if (read_size > 0 && strncmp(identificacion,"nuevo",5)==0){
								cantidad_nodos++;
								cantidad_nodos_historico=cantidad_nodos;
								FD_SET(newfd, &master);
								fdmax = newfd;
								list_add (nodos, agregar_nodo_a_lista(newfd,asignar_nombre_a_nodo(),0,inet_ntoa(remote_client.sin_addr),remote_client.sin_port,50,50));
								printf ("Se conecto el nodo %s\n",inet_ntoa(remote_client.sin_addr));
							}
							if (read_size > 0 && strncmp(identificacion,"reconectado",11)==0){
								cantidad_nodos++;
								FD_SET(newfd, &master);
								fdmax = newfd;
								modificar_estado_nodo (i,inet_ntoa(remote_client.sin_addr),remote_client.sin_port,1); //cambio su estado de la lista a 1 que es activo
								printf ("Se conecto el nodo %s\n",inet_ntoa(remote_client.sin_addr));
							}
						}
					}
					printf("select: conexion desde %s en socket %d\n", inet_ntoa(remote_client.sin_addr),newfd);


				//.................................................
				//hasta aca, es el tratamiento de conexiones nuevas
				//.................................................

				} else { //si entra aca no es un cliente nuevo, es uno que ya tenia y me esta mandando algo
					// gestionar datos de un cliente
					if ((nbytes = recv(i, mensaje, sizeof(mensaje), 0)) <= 0) { //si entra aca es porque se desconecto o hubo un error
						if (nbytes == 0) {
							// Un nodo cerro su conexion, actualizo la lista de nodos
							//............................
							//............................
							modificar_estado_nodo (i,NULL,-1,0);

						} else {
							perror("recv");
							log_info(logger,"FALLO el Recv");
							exit(-1);
						}
						close(i); // ¡Hasta luego!
						FD_CLR(i, &master); // eliminar del conjunto maestro
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
uint32_t BuscarPadre (char* path){
//    t_dir directorio;
//    t_list listaDir;


return 0;
}


//Buscar la posición del nodo de un archivo de la lista t_archivo por el nombre del archivo y el id del padre
uint32_t BuscarArchivoPorNombre (const char *path, uint32_t padre){
    t_archivo* archivo;
    t_list* listaArchivos;
    char** nombreArchivo = string_split((char*) path, "/");
    int posArchivo = 0;
    int tam = list_size(listaArchivos);
    int i;
    for(i=0; i<tam;i++){
        archivo = list_get(listaArchivos,posArchivo);
        if (strcmp(archivo->nombre,nombreArchivo)&(archivo->padre == padre)){
            return posArchivo;
            break;
        }
    }
}

char *asignar_nombre_a_nodo(void){
	char *nombre_temporal;
	char *numero_nodo = malloc (1);
	sprintf(numero_nodo,"%d",cantidad_nodos_historico);
	nombre_temporal=malloc(4+strlen(numero_nodo));
	strcat(nombre_temporal,"nodo");
	strcat(nombre_temporal,numero_nodo);
	return nombre_temporal;
}

void modificar_estado_nodo (int socket,char *ip,int port,int estado){
	int i;
	t_nodo *tmp;
	for (i=0;i<list_size(nodos);i++){
		tmp = list_get(nodos,i);
		if ((ip==NULL && port==-1) && tmp->socket==socket)
			tmp->estado=estado;
		else if ((strcmp(tmp->ip,ip)==1) && tmp->puerto==port){
			tmp->estado=estado;
			tmp->socket=socket;
		}
	}
}
void formatear_nodos(){
	int i;
	for (i=0;i<list_size(nodos);i++)
		list_remove(nodos,i);
}


void FormatearFilesystem (){
	printf("Eligió  Formatear el MDFS\n");
}

void EliminarArchivo(){
    printf("Eligió  Eliminar archivo\n");
//    char* path;
//    t_list listaArchivos;
//    t_list listaBloques;
//    t_bloque bloque;
//    printf ("Ingrese el path del archivo \n");
//    scanf ("%s", path);
//     uint32_t idPadre = BuscarPadre(path);
//    uint32_t posArchivo = BuscarArchivoPorNombre (path,idPadre);
//    t_archivo archivo = list_get(listaArchivos,posArchivo);
//    listaBloques= archivo.bloques;
//    //Eliminar bloques del archivo
//    while(listaBloques!=NULL){
//        list_destroy_and_destroy_elements(listaBloques, void(bloque)(void*));
//    }
//    //Elimnar nodo del archivo t_arhivo
//    list_remove_and_destroy_element(listaArchivos, posArchivo, void(archivo)(void*));
}

void RenombrarArchivo (){
	printf("Eligió Renombrar archivos\n");
}

void MoverArchivo(){
	 printf("Eligió Mover archivos\n");
}

void CrearDirectorio(){
	printf("Eligió Crear directorios\n");
}

void EliminarDirectorio(){
	printf("Eligió Eliminar directorios\n");
}

void RenombrarDirectorio(){
	printf("Eligió Renombrar directorios\n");
}

void MoverDirectorio(){
	printf("Eligió Mover directorios\n");
}

void CopiarArchivoAMDFS(){
	printf("Eligió Copiar un archivo local al MDFS\n");
}

void CopiarArchivoDelMDFS(){
	printf("Eligió Copiar un archivo del MDFS al filesystem local\n");
}

void MD5DeArchivo(){
	printf("Eligió Solicitar el MD5 de un archivo en MDFS\n");
}

void VerBloques(){
	printf("Eligió Ver los bloques que componen un archivo\n");
}

void BorrarBloques(){
	printf("Eligió Borrar los bloques que componen un archivo\n");
}

void CopiarBloques(){
	printf("Eligió Copiar los bloques que componen un archivo\n");
}

void AgregarNodo(){
	printf("Eligió Agregar un nodo de datos\n");
}

void EliminarNodo(){
	printf("Eligió Eliminar un nodo de datos\n");
}
