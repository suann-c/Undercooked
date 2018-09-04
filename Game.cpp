#include "Game.hpp"

#include "gl_errors.hpp" //helper for dumpping OpenGL error messages
#include "read_chunk.hpp" //helper for reading a vector of structures from a file
#include "data_path.hpp" //helper to get paths relative to executable

#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <fstream>
#include <map>
#include <cstddef>
#include <random>

//helper defined later; throws if shader compilation fails:
static GLuint compile_shader(GLenum type, std::string const &source);

Game::Game() {
	{ //create an opengl program to perform sun/sky (well, directional+hemispherical) lighting:
		GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER,
			"#version 330\n"
			"uniform mat4 object_to_clip;\n"
			"uniform mat4x3 object_to_light;\n"
			"uniform mat3 normal_to_light;\n"
			"layout(location=0) in vec4 Position;\n" //note: layout keyword used to make sure that the location-0 attribute is always bound to something
			"in vec3 Normal;\n"
			"in vec4 Color;\n"
			"out vec3 position;\n"
			"out vec3 normal;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"	gl_Position = object_to_clip * Position;\n"
			"	position = object_to_light * Position;\n"
			"	normal = normal_to_light * Normal;\n"
			"	color = Color;\n"
			"}\n"
		);

		GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
			"#version 330\n"
			"uniform vec3 sun_direction;\n"
			"uniform vec3 sun_color;\n"
			"uniform vec3 sky_direction;\n"
			"uniform vec3 sky_color;\n"
			"in vec3 position;\n"
			"in vec3 normal;\n"
			"in vec4 color;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"	vec3 total_light = vec3(0.0, 0.0, 0.0);\n"
			"	vec3 n = normalize(normal);\n"
			"	{ //sky (hemisphere) light:\n"
			"		vec3 l = sky_direction;\n"
			"		float nl = 0.5 + 0.5 * dot(n,l);\n"
			"		total_light += nl * sky_color;\n"
			"	}\n"
			"	{ //sun (directional) light:\n"
			"		vec3 l = sun_direction;\n"
			"		float nl = max(0.0, dot(n,l));\n"
			"		total_light += nl * sun_color;\n"
			"	}\n"
			"	fragColor = vec4(color.rgb * total_light, color.a);\n"
			"}\n"
		);

		simple_shading.program = glCreateProgram();
		glAttachShader(simple_shading.program, vertex_shader);
		glAttachShader(simple_shading.program, fragment_shader);
		//shaders are reference counted so this makes sure they are freed after program is deleted:
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);

		//link the shader program and throw errors if linking fails:
		glLinkProgram(simple_shading.program);
		GLint link_status = GL_FALSE;
		glGetProgramiv(simple_shading.program, GL_LINK_STATUS, &link_status);
		if (link_status != GL_TRUE) {
			std::cerr << "Failed to link shader program." << std::endl;
			GLint info_log_length = 0;
			glGetProgramiv(simple_shading.program, GL_INFO_LOG_LENGTH, &info_log_length);
			std::vector< GLchar > info_log(info_log_length, 0);
			GLsizei length = 0;
			glGetProgramInfoLog(simple_shading.program, GLsizei(info_log.size()), &length, &info_log[0]);
			std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
			throw std::runtime_error("failed to link program");
		}
	}

	{ //read back uniform and attribute locations from the shader program:
		simple_shading.object_to_clip_mat4 = glGetUniformLocation(simple_shading.program, "object_to_clip");
		simple_shading.object_to_light_mat4x3 = glGetUniformLocation(simple_shading.program, "object_to_light");
		simple_shading.normal_to_light_mat3 = glGetUniformLocation(simple_shading.program, "normal_to_light");

		simple_shading.sun_direction_vec3 = glGetUniformLocation(simple_shading.program, "sun_direction");
		simple_shading.sun_color_vec3 = glGetUniformLocation(simple_shading.program, "sun_color");
		simple_shading.sky_direction_vec3 = glGetUniformLocation(simple_shading.program, "sky_direction");
		simple_shading.sky_color_vec3 = glGetUniformLocation(simple_shading.program, "sky_color");

		simple_shading.Position_vec4 = glGetAttribLocation(simple_shading.program, "Position");
		simple_shading.Normal_vec3 = glGetAttribLocation(simple_shading.program, "Normal");
		simple_shading.Color_vec4 = glGetAttribLocation(simple_shading.program, "Color");
	}

	struct Vertex {
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::u8vec4 Color;
	};
	static_assert(sizeof(Vertex) == 28, "Vertex should be packed.");

	{ //load mesh data from a binary blob:
		std::ifstream blob(data_path("meshes.blob"), std::ios::binary);
		//The blob will be made up of three chunks:
		// the first chunk will be vertex data (interleaved position/normal/color)
		// the second chunk will be characters
		// the third chunk will be an index, mapping a name (range of characters) to a mesh (range of vertex data)

		//read vertex data:
		std::vector< Vertex > vertices;
		read_chunk(blob, "dat0", &vertices);

		//read character data (for names):
		std::vector< char > names;
		read_chunk(blob, "str0", &names);

		//read index:
		struct IndexEntry {
			uint32_t name_begin;
			uint32_t name_end;
			uint32_t vertex_begin;
			uint32_t vertex_end;
		};
		static_assert(sizeof(IndexEntry) == 16, "IndexEntry should be packed.");

		std::vector< IndexEntry > index_entries;
		read_chunk(blob, "idx0", &index_entries);

		if (blob.peek() != EOF) {
			std::cerr << "WARNING: trailing data in meshes file." << std::endl;
		}

		//upload vertex data to the graphics card:
		glGenBuffers(1, &meshes_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		//create map to store index entries:
		std::map< std::string, Mesh > index;
		for (IndexEntry const &e : index_entries) {
			if (e.name_begin > e.name_end || e.name_end > names.size()) {
				throw std::runtime_error("invalid name indices in index.");
			}
			if (e.vertex_begin > e.vertex_end || e.vertex_end > vertices.size()) {
				throw std::runtime_error("invalid vertex indices in index.");
			}
			Mesh mesh;
			mesh.first = e.vertex_begin;
			mesh.count = e.vertex_end - e.vertex_begin;
			auto ret = index.insert(std::make_pair(
				std::string(names.begin() + e.name_begin, names.begin() + e.name_end),
				mesh));
			if (!ret.second) {
				throw std::runtime_error("duplicate name in index.");
			}
		}

		//look up into index map to extract meshes:
		auto lookup = [&index](std::string const &name) -> Mesh {
			auto f = index.find(name);
			if (f == index.end()) {
				throw std::runtime_error("Mesh named '" + name + "' does not appear in index.");
			}
			return f->second;
		};
		//CHANGED (removed cursor)
		tile_mesh = lookup("Tile");
		//cursor_mesh = lookup("Cursor");
		doll_mesh = lookup("Doll");
		bread_mesh = lookup("bread");
		pb_mesh = lookup("PB");
		j_mesh = lookup("J");
		cube_mesh = lookup("Cube");
	}

	{ //create vertex array object to hold the map from the mesh vertex buffer to shader program attributes:
		glGenVertexArrays(1, &meshes_for_simple_shading_vao);
		glBindVertexArray(meshes_for_simple_shading_vao);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		//note that I'm specifying a 3-vector for a 4-vector attribute here, and this is okay to do:
		glVertexAttribPointer(simple_shading.Position_vec4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Position));
		glEnableVertexAttribArray(simple_shading.Position_vec4);
		if (simple_shading.Normal_vec3 != -1U) {
			glVertexAttribPointer(simple_shading.Normal_vec3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Normal));
			glEnableVertexAttribArray(simple_shading.Normal_vec3);
		}
		if (simple_shading.Color_vec4 != -1U) {
			glVertexAttribPointer(simple_shading.Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Color));
			glEnableVertexAttribArray(simple_shading.Color_vec4);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	GL_ERRORS();

	//initialize everything
	initBoard();
}

void Game::initBoard() {
	//----------------
	//set up game board with meshes and rolls:
	board_meshes.reserve(board_size.x * board_size.y);
	board_rotations.reserve(board_size.x * board_size.y);
	//std::mt19937 mt(0xbead1234);

	//global win condition
	struct win;

	//initialize chef.x chef.x (for second and onward rounds)
	chef.x = 2;
	chef.y = 2;

	//initialize certain squares. 0 means empty square, 1 means square 
	//with chef in it, 2 is square with jelly, 3 is square with peanut
	//butter, 4 for square with bread, 5 for goal square and 6 for empty
	//side squares.
	//four corner squares are zeros
	// init board
	for (uint32_t y = 0; y < board_size.y; ++y) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			if (x == 2 and y == 2) {
				board[x][y] = 1; // set chef position
			}
			else {
				board[x][y] = 0; // init other positions
								 // set outside board 
				if ((y == 0 and (x>0 and x<4)) || (y == 4 and (x>0 and x<4)) || 
					(x == 0 and (y>0 and y<4)) || (x == 4 and (y>0 and y<4))) {
					board[x][y] = 6; // init other positions
				}
			}
		}
	}

	//Game::spawnFood to add food randomly to the surrounding squares
	std::vector <std::tuple<int, int>> fillIn;
	//put in stuff to vector
	fillIn.push_back(std::tuple<int, int>(0,1));
	fillIn.push_back(std::tuple<int, int>(0,2));
	fillIn.push_back(std::tuple<int, int>(0,3));
	fillIn.push_back(std::tuple<int, int>(1,0));
	fillIn.push_back(std::tuple<int, int>(2,0));
	fillIn.push_back(std::tuple<int, int>(3,0));
	fillIn.push_back(std::tuple<int, int>(4,1));
	fillIn.push_back(std::tuple<int, int>(4,2));
	fillIn.push_back(std::tuple<int, int>(4,3));
	fillIn.push_back(std::tuple<int, int>(1,4));
	fillIn.push_back(std::tuple<int, int>(2,4));
	fillIn.push_back(std::tuple<int, int>(3,4));
	Game::spawnFood(fillIn);

	std::vector< Mesh const * > meshes{&doll_mesh, &pb_mesh, &cube_mesh, &bread_mesh, &j_mesh};

	//initializing board_meshes
	int val;
	for (int i=0; i<board_size.x; i++) {
		for (int j=0; j<board_size.y; j++) {
			val = board[i][j];
			int ind = i*board_size.x + j;
			//std::cout << "val is " << val << std::endl;
			if (val == 1) { //draw person
				board_meshes[ind] = meshes[0];
			}
			else if (val > 1 and val < 6) { 
				//draw generic item (PB,J,bread,goal)
				if (val == 2) { //j
					board_meshes[ind] = meshes[4];
				}
				else if (val == 3) { //pb
					board_meshes[ind] = meshes[1];
				}
				else if (val == 4) { //bread
					board_meshes[ind] = meshes[3];
				}
				else { //goal
					board_meshes[ind] = meshes[2];
				}
			}
			else {
				board_meshes[ind] = nullptr;
			}
			board_rotations.emplace_back(glm::quat());
		}
	}
} 


Game::~Game() {
	glDeleteVertexArrays(1, &meshes_for_simple_shading_vao);
	meshes_for_simple_shading_vao = -1U;

	glDeleteBuffers(1, &meshes_vbo);
	meshes_vbo = -1U;

	glDeleteProgram(simple_shading.program);
	simple_shading.program = -1U;

	GL_ERRORS();
}

//CHANGED (coded spawnFood, and getFood)
void Game::spawnFood(std::vector <std::tuple<int, int>> counterSpace) {
	srand(time(NULL));
	//fill in all 12 spaces around the chef with something
	int x;
	int y;
	int ind;
	int len = counterSpace.size();
	for (int i=0; i<4; i++) {
		//randomly pick one from list
		ind = rand()%len;
		//std::cout << "random val is " << ind << std::endl;
		x = std::get<0>(counterSpace[ind]);
		y = std::get<1>(counterSpace[ind]);
		//std::cout << "x is " << x << std::endl;
		//std::cout << "y is " << y << std::endl;
		if (i == 0) { //pick place for PB
			board[x][y] = 3;
		}
		else if (i == 1) { //pick place for J
			board[x][y] = 2;
		}
		else if (i == 2) { //pick place for bread
			board[x][y] = 4;
		}
		else { //pick a place for goal square
			board[x][y] = 5;
		}
		len -= 1; //so when picking again there is no out of range
		//delete it entirely from vector
		counterSpace.erase(counterSpace.begin()+ind);
	}
}

void Game::getFood(int dir) {
	int item;
	int x;
	int y;
	if (dir == 0) { //pick up something a row above
		//check board[chefRow-1][chefCol] for item
		x = chef.x-1;
		y = chef.y;
		item = board[x][y];
	}
	else if (dir == 1) { //pick up something a row below
		//check board[chefRow+1][chefCol] for item
		x = chef.x+1;
		y = chef.y;
		item = board[x][y];
	}
	else if (dir == 2) { //pick up something to the left
		//check board[chefRow][chefCol-1] for item
		x = chef.x;
		y = chef.y-1;
		item = board[x][y];
	}
	else { //dir == 3 aka pick up something to the right
		//check board[chefRow][chefCol+1] for item
		x = chef.x;
		y = chef.y+1;
		item = board[x][y];
	}
	if (item > 1 and item < 6) { //non empty and non illegal
		if (item == 5) { //goal square
			if (win.PB == 1 and win.J == 1 and win.bread == 1) {
				//round won! reset some variables
				//set everything to zero for second and onward rounds
				win.PB = 0;
				win.J = 0;
				win.bread = 0;
				initBoard();
			}
		}
		else {
			if (item == 3) {//PB
				win.PB = 1;
			}
			else if (item == 2) {//J
				win.J = 1;
			}
			else {//bread
				win.bread = 1;
			}
			//update the board
			board[x][y] = 6;
			int ind = board_size.x*x+y;
			board_meshes[ind] = nullptr;
		}
	}
}

void Game::printouts() {
	std::cout << "chef.x is: " << chef.x << " and chef.y is: "<< chef.y << std::endl;
	//print out the board
	for (int i=0; i<5; i++) {
		for (int j=0; j<5; j++) {
			std::cout<<"board at "<<i<<", "<<j<<"is: "<<board[i][j]<<std::endl;
		}
	}
}

bool Game::handle_event(SDL_Event const &evt, glm::uvec2 window_size) {
	//ignore any keys that are the result of automatic key repeat:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat) {
		return false;
	}
	//move chef on L/R/U/D press:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat == 0) {
		//move chef one square to or pick up item
		if (evt.key.keysym.scancode == SDL_SCANCODE_UP) { //up arrow pressed
			if (chef.x == 3) { //call getFood
				getFood(1);
			}
			if (chef.x < 3) { //move chef one row down
				board[chef.x][chef.y] = 0;
				int oldInd = board_size.x*chef.x + chef.y;
				board_meshes[oldInd] = nullptr;
				chef.x += 1;
				board[chef.x][chef.y] = 1; //move chef's representation on board
				int newInd = board_size.x*chef.x + chef.y;
				board_meshes[newInd] = &doll_mesh;
			}
			return true;
		}
		else if (evt.key.keysym.scancode == SDL_SCANCODE_DOWN) { //down arrow pressed
			if (chef.x == 1) { //call getFood
				getFood(0);
			}
			if (chef.x > 1) { //move chef one row up
				board[chef.x][chef.y] = 0;
				int oldInd = board_size.x*chef.x + chef.y;
				board_meshes[oldInd] = nullptr;
				chef.x -= 1;
				board[chef.x][chef.y] = 1; //move chef's representation on board
				int newInd = board_size.x*chef.x + chef.y;
				board_meshes[newInd] = &doll_mesh;
			}
			return true;
		}
		else if (evt.key.keysym.scancode == SDL_SCANCODE_LEFT) { //left arrow pressed
			if (chef.y == 1) { //call getFood
				getFood(2);
			}
			if (chef.y > 1) { //move chef one col left
				board[chef.x][chef.y] = 0;
				int oldInd = board_size.x*chef.x + chef.y;
				board_meshes[oldInd] = nullptr;
				chef.y -= 1;
				board[chef.x][chef.y] = 1; //move chef's representation on board
				int newInd = board_size.x*chef.x + chef.y;
				board_meshes[newInd] = &doll_mesh;
			}
			return true;
		}
		else if (evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) { //right arrow pressed
			if (chef.y == 3) { //call getFood
				getFood(3);
			}
			if (chef.y < 3) { //move chef one col right
				board[chef.x][chef.y] = 0;
				int oldInd = board_size.x*chef.x + chef.y;
				board_meshes[oldInd] = nullptr;
				chef.y += 1;
				board[chef.x][chef.y] = 1; //move chef's representation on board
				int newInd = board_size.x*chef.x + chef.y;
				board_meshes[newInd] = &doll_mesh;
			}
			return true;
		}
	}
	return false;
}

void Game::update(float elapsed) {
	/*
	glm::quat dr = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	float amt = elapsed * 1.0f;
	if (controls.roll_left) {
		dr = glm::angleAxis(amt, glm::vec3(0.0f, 1.0f, 0.0f)) * dr;
	}
	if (controls.roll_right) {
		dr = glm::angleAxis(-amt, glm::vec3(0.0f, 1.0f, 0.0f)) * dr;
	}
	if (controls.roll_up) {
		dr = glm::angleAxis(amt, glm::vec3(1.0f, 0.0f, 0.0f)) * dr;
	}
	if (controls.roll_down) {
		dr = glm::angleAxis(-amt, glm::vec3(1.0f, 0.0f, 0.0f)) * dr;
	}
	if (dr != glm::quat()) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			glm::quat &r = board_rotations[cursor.y * board_size.x + x];
			r = glm::normalize(dr * r);
		}
		for (uint32_t y = 0; y < board_size.y; ++y) {
			if (y != cursor.y) {
				glm::quat &r = board_rotations[y * board_size.x + cursor.x];
				r = glm::normalize(dr * r);
			}
		}
	}
	*/
}

void Game::draw(glm::uvec2 drawable_size) {
	//Set up a transformation matrix to fit the board in the window:
	glm::mat4 world_to_clip;
	{
		float aspect = float(drawable_size.x) / float(drawable_size.y);

		//want scale such that board * scale fits in [-aspect,aspect]x[-1.0,1.0] screen box:
		float scale = glm::min(
			2.0f * aspect / float(board_size.x),
			2.0f / float(board_size.y)
		);

		//center of board will be placed at center of screen:
		glm::vec2 center = 0.5f * glm::vec2(board_size);

		//NOTE: glm matrices are specified in column-major order
		world_to_clip = glm::mat4(
			scale / aspect, 0.0f, 0.0f, 0.0f,
			0.0f, scale, 0.0f, 0.0f,
			0.0f, 0.0f,-1.0f, 0.0f,
			-(scale / aspect) * center.x, -scale * center.y, 0.0f, 1.0f
		);
	}

	//set up graphics pipeline to use data from the meshes and the simple shading program:
	glBindVertexArray(meshes_for_simple_shading_vao);
	glUseProgram(simple_shading.program);

	glUniform3fv(simple_shading.sun_color_vec3, 1, glm::value_ptr(glm::vec3(0.81f, 0.81f, 0.76f)));
	glUniform3fv(simple_shading.sun_direction_vec3, 1, glm::value_ptr(glm::normalize(glm::vec3(-0.2f, 0.2f, 1.0f))));
	glUniform3fv(simple_shading.sky_color_vec3, 1, glm::value_ptr(glm::vec3(0.2f, 0.2f, 0.3f)));
	glUniform3fv(simple_shading.sky_direction_vec3, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));

	//helper function to draw a given mesh with a given transformation:
	auto draw_mesh = [&](Mesh const &mesh, glm::mat4 const &object_to_world) {
		//set up the matrix uniforms:
		if (simple_shading.object_to_clip_mat4 != -1U) {
			glm::mat4 object_to_clip = world_to_clip * object_to_world;
			glUniformMatrix4fv(simple_shading.object_to_clip_mat4, 1, GL_FALSE, glm::value_ptr(object_to_clip));
		}
		if (simple_shading.object_to_light_mat4x3 != -1U) {
			glUniformMatrix4x3fv(simple_shading.object_to_light_mat4x3, 1, GL_FALSE, glm::value_ptr(object_to_world));
		}
		if (simple_shading.normal_to_light_mat3 != -1U) {
			//NOTE: if there isn't any non-uniform scaling in the object_to_world matrix, then the inverse transpose is the matrix itself, and computing it wastes some CPU time:
			glm::mat3 normal_to_world = glm::inverse(glm::transpose(glm::mat3(object_to_world)));
			glUniformMatrix3fv(simple_shading.normal_to_light_mat3, 1, GL_FALSE, glm::value_ptr(normal_to_world));
		}

		//draw the mesh:
		glDrawArrays(GL_TRIANGLES, mesh.first, mesh.count);
	};

	for (uint32_t y = 0; y < board_size.y; ++y) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			draw_mesh(tile_mesh,
				glm::mat4(
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					x+0.5f, y+0.5f,-0.5f, 1.0f
				)
			);
			int val = board[y][x];
			//std::cout << "val is " << val << std::endl;
			if (val==1 || val==2 || val==3 || val==4 || val==5 ) {
				draw_mesh(*board_meshes[y*board_size.x+x],
					glm::mat4(
						1.0f, 0.0f, 0.0f, 0.0f,
						0.0f, 1.0f, 0.0f, 0.0f,
						0.0f, 0.0f, 1.0f, 0.0f,
						x+0.5f, y+0.5f, 0.0f, 1.0f
					)
					* glm::mat4_cast(board_rotations[y*board_size.x+x])
				);
			}
		}
	}

	glUseProgram(0);

	GL_ERRORS();
}



//create and return an OpenGL vertex shader from source:
static GLuint compile_shader(GLenum type, std::string const &source) {
	GLuint shader = glCreateShader(type);
	GLchar const *str = source.c_str();
	GLint length = GLint(source.size());
	glShaderSource(shader, 1, &str, &length);
	glCompileShader(shader);
	GLint compile_status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
	if (compile_status != GL_TRUE) {
		std::cerr << "Failed to compile shader." << std::endl;
		GLint info_log_length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetShaderInfoLog(shader, GLsizei(info_log.size()), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		glDeleteShader(shader);
		throw std::runtime_error("Failed to compile shader.");
	}
	return shader;
}
