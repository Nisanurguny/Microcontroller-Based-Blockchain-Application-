Microcontroller-Based Blockchain ApplicationThis project introduces a lightweight and scalable "Micro-Blockchain" architecture capable of simultaneously ensuring distributed data integrity and security on resource-constrained Atmega-based embedded systems. 
While most current blockchain applications run on software platforms that require high hardware capacity, this study reinterprets the core principles of blockchain technology—distribution, integrity, and cryptographic security—directly at the microcontroller level and with a hardware-centric approach.  

🛠️ System Architecture and Hardware StructureThe system consists of an input controller, a node network made up of 4-layer encryption stages, and an output controller. 
Main Input Controller: 1 x Arduino Mega 2560 (Atmega2560). It receives the data input, divides it into shards, and distributes them to the first layer.  
Intermediate Layer Nodes: 32 x Arduino Uno (Atmega328P). In each encryption layer, 8 nodes operate in parallel to process the incoming data shards. 
Output Controller: 1 x Arduino Mega 2560. It collects the encrypted and transformed shards from the final layer to combine them into the definitive output file.  

📡 Communication Infrastructure: UART (RX/TX) Serial CommunicationInter-node communication in the project is built upon an asynchronous serial communication (UART) architecture. Unlike protocols that offer hardware-based addressing like I²C, this structure operates a software-defined and secure communication protocol:  
Software Addressing: Each data packet is transmitted with a specific Header structure that contains the unique identity of the target Arduino Uno.  
Flow Control: The data flow is designed as unidirectional. A software-based ACK/NACK (Acknowledgment/Negative-Acknowledgment) mechanism and handshaking protocol are utilized to ensure data integrity and verify packet transmission.  Timing and Synchronization: To prevent collisions, all nodes are synchronized to a high-speed standard Baud Rate, and data transmission times are software-coordinated.  

🔐 Layer-Based Encryption Process
The data arriving at the input controller is divided into 8 equal parts (shards). Each shard is subjected to lightweight cryptographic transformations at the bit and byte level across 4 different layers: 

Layer 1: Position-Based Bit FlippingA unique bit position is calculated by blending the unique shard ID (shard_id), layer ID (LID), and time stamp (timestamp) parameters of the input shard. The bit at this position is logically inverted using a NOT operation. At the end of the process, the CRC32 integrity value of the shard is calculated and appended to the data header.  

Layer 2: Circular Bit ShiftingEach shard is circularly shifted to the right by $r$ positions, determined according to the identity value (node_id) of the respective node. Aiming for low energy consumption, this process is coordinated at the edge using the bitwise shift register method. 

Layer 3: Lightweight XOR-Based EncryptionAt each node, an 8-bit long pseudo-random key (pseudo-key) is generated using a Linear Feedback Shift Register (LFSR). This key is applied to the data shard via the XOR logical operation, increasing the statistical randomness of the data and breaking the correlation between layers. 

Layer 4: Byte Permutation and Parity UpdateIn the final layer, data bytes are rearranged (permuted) according to a fixed and small permutation table pre-embedded into the hardware. Following this, the parity bit is updated, and the final CRC32 field is recalculated to complete the chain.  

Chained Integrity (Hash Chain Logic): The CRC value generated in each layer is incorporated into the process as the previous CRC (prev_crc) parameter in the next layer; thereby forming an unbreakable chained integrity structure between the layers.  

📊 Measurement and Performance Metrics
The viability of this developed "Micro-Blockchain" prototype regarding embedded systems is measured instantaneously during each operational cycle and logged to a computer via the serial port (UART):  

Latency (t and T): Processing latencies per layer and across the overall system are measured using millis() timers.
Energy Consumption (E): Power and average current consumption per node are calculated instantaneously through the integration of the INA219 current/voltage sensor module. 
Cryptographic Performance: Hamming Distance (HD) is verified with software functions to measure data differentiation between input and output, alongside Shannon Entropy (H) for randomness analysis, and Bit Error Rate (BER) for shard integrity.  Memory Usage: SRAM and Flash memory consumption of system components are optimized through compilation reports.  

🎯 Aim and Objectives of the Project
Physical Blockchain Modeling: Rendering abstract blockchain concepts (distributed ledger, integrity control, consensus) visible on physical hardware, moving beyond mere simulations.  
Educational Kit Design: Creating a low-cost and accessible blockchain educational kit that can be utilized in engineering and cybersecurity education.  
IoT Security: Defining a lightweight security infrastructure for secure data sharing among edge devices in future Internet of Things and industrial embedded system (ICS) networks.  

Academic Credentials
This work is a Bachelor's Graduation Project prepared within the Department of Computer Engineering, Faculty of Engineering at Van Yüzüncü Yıl University.  Advisor: Prof. Dr. Rıdvan SARAÇOĞLU  Developer: Nisanur GÜNEY  Report Date: June, 2026
