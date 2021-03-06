#define BUF_SIZE 15
#define REDUCE_SIZE 8192
#define MAPPER_SIZE 8192
#define MENSAJE_SIZE 4096
#define TAM_NOMFINAL 60

typedef struct estructura_mapper {
	char ip_nodo[20];
	int puerto_nodo;
	int bloque;
	char archivoResultadoMap[TAM_NOMFINAL];
} __attribute__((packed)) t_mapper;

typedef struct datos_para_map{
	uint32_t bloque;
	char nomArchTemp[TAM_NOMFINAL];
	char rutinaMap[MAPPER_SIZE];
} __attribute__((packed)) t_datosMap;


typedef struct estructura_reduce {
	char ip_nodoPpal[20];
	int puerto_nodoPpal;
	char nombreArchivoFinal[TAM_NOMFINAL];
} __attribute__((packed)) t_reduce;

typedef struct lista_nodos_reduce{
	char ip_nodo[20];
	int puerto_nodo;
	char archivoAAplicarReduce[TAM_NOMFINAL];
} __attribute__((packed)) t_archivosReduce;

typedef struct estructura_hilo_reduce {
	char ip_nodoPpal[20];
	int puerto_nodoPpal;
	t_list* listaNodos; //una lista que tenga los otros nodos y archivos a donde aplicar reduce (lista de t_reduce_otrosnodos)
	char nombreArchivoFinal[TAM_NOMFINAL];
} t_hiloReduce;

//Estrcutura que va a mandar el job a marta cuando termine un map
typedef struct estructura_respuesta {
	char archivoResultadoMap[TAM_NOMFINAL];
	int resultado; // 0 si salio bien , y 1 si salio mal el map
}__attribute__((packed)) t_respuestaMap;

typedef struct estructura_respuesta_reduce{
	int resultado;
	char archivoResultadoReduce[TAM_NOMFINAL];
	char ip_nodo[20]; //Puede ser el principal, o uno que fallo
	int puerto_nodo; //Puede ser el principal, o uno que fallo
}__attribute__((packed)) t_respuestaReduce;

typedef struct estructura_respuesta_reduce_delnodo{
	int resultado;
	char ip_nodoFallido[20];
	int puerto_nodoFallido;
}__attribute__((packed)) t_respuestaNodoReduce;


//Declaración de funciones
void* hilo_mapper(t_mapper*);
void* hilo_reduce(t_hiloReduce*);
char* getFileContent(char*); //Devuelve el contenido de un file, hasta 8192 bytes -> 8 KB (MAPPER_SIZE)
